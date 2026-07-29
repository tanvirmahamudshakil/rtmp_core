#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rtmp_server/authentication/rtmp_authenticator.hpp"
#include "rtmp_server/control/http_server.hpp"
#include "rtmp_server/control/management_api.hpp"
#include "rtmp_server/core/config.hpp"
#include "rtmp_server/io/io_uring/worker_pool.hpp"
#include "rtmp_server/management/stream_manager.hpp"
#include "rtmp_server/observability/audit_log.hpp"
#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/persistence/sqlite_store.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"

namespace {

rtmp_server::io::io_uring::WorkerPool* g_pool = nullptr;

// Signal handlers must be async-signal-safe: only touch an atomic/flag and
// return. Actual teardown happens on each worker's own thread inside its
// run(), per docs/rtmp_promot.md "Graceful Shutdown".
void handle_shutdown_signal(int /*signum*/) {
    if (g_pool != nullptr) g_pool->stop();
}

void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = handle_shutdown_signal;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    // Prevent SIGPIPE from killing the process on writes to a peer that
    // already reset the connection (docs/rtmp_promot.md "io_uring Error
    // Classification").
    ::signal(SIGPIPE, SIG_IGN);
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "./config/server.yaml";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    auto config_result = rtmp_server::core::load_config(config_path);
    if (!config_result) {
        std::fprintf(stderr, "failed to load config '%s': %s\n", config_path.c_str(),
                      config_result.error().message().c_str());
        return EXIT_FAILURE;
    }

    using rtmp_server::observability::LogLevel;
    auto& logger = rtmp_server::observability::Logger::instance();
    if (config_result.value().log_level == "debug") logger.set_level(LogLevel::Debug);
    else if (config_result.value().log_level == "trace") logger.set_level(LogLevel::Trace);
    else if (config_result.value().log_level == "warn") logger.set_level(LogLevel::Warn);
    else if (config_result.value().log_level == "error") logger.set_level(LogLevel::Error);

    const auto& config = config_result.value();

    if (config.database_type != "sqlite") {
        std::fprintf(stderr, "unsupported database_type '%s' (this single-node build requires sqlite)\n",
                     config.database_type.c_str());
        return EXIT_FAILURE;
    }

    auto store_result = rtmp_server::persistence::SqliteStore::open(config.database_connection);
    if (!store_result) {
        std::fprintf(stderr, "failed to open database '%s': %s\n", config.database_connection.c_str(),
                     store_result.error().message().c_str());
        return EXIT_FAILURE;
    }
    auto store = std::move(store_result).value();

    rtmp_server::observability::Metrics metrics;
    rtmp_server::observability::AuditLog audit_log;
    rtmp_server::management::StreamManager stream_manager(
        {config.public_rtmp_hostname, config.rtmp_port, config.token_signing_secret});
    stream_manager.set_store(store.get());
    stream_manager.set_audit_log(&audit_log);
    stream_manager.set_metrics(&metrics);
    auto load_result = stream_manager.load_from_store();
    if (!load_result) {
        std::fprintf(stderr, "failed to load persisted stream configuration: %s\n",
                     load_result.error().message().c_str());
        return EXIT_FAILURE;
    }

    rtmp_server::authentication::AuthenticatorLimits auth_limits;
    auth_limits.max_publishers_per_stream = 1;
    auth_limits.max_viewers_per_stream = config.maximum_viewers_per_stream;
    auth_limits.max_connections_per_ip = config.maximum_connections_per_ip;
    rtmp_server::authentication::RtmpAuthenticator authenticator(stream_manager, auth_limits);
    authenticator.set_metrics(&metrics);

    // Process-wide RTMP stream identity tables (Phase 1), shared by
    // reference across every WorkerPool worker so a publish/playback name
    // resolves to the same StreamId regardless of which worker's ring
    // accepted the connection (docs/v2_promot.md PHASE 4 task "avoid
    // cross-worker access to mutable connection state" — these two are the
    // one deliberate, mutex-guarded exception, by design; see
    // IoUringEventLoop's class doc).
    rtmp_server::protocol::commands::StreamRegistry stream_registry;
    rtmp_server::protocol::commands::StreamIdRegistry stream_id_registry;
    stream_registry.set_metrics(&metrics);

    rtmp_server::io::io_uring::EventLoopServices services;
    services.metrics = &metrics;
    // Open publishing: OBS/ffmpeg publishes with the public stream name,
    // not a secret credential. Requiring an existing enabled application and
    // stream preserves deterministic routing and the operator's disable
    // control without adding authentication.
    services.key_validator =
        [&stream_manager](std::string_view application, std::string_view stream_name) {
            const auto app = stream_manager.find_application(application);
            const auto stream = stream_manager.find_stream(application, stream_name);
            return app && app->enabled && stream && stream->enabled;
        };
    // The publish argument is already the canonical public fan-out name.
    services.stream_id_resolver = {};
    services.playback_authorizer = authenticator.playback_authorizer();
    services.viewer_attached_handler =
        [&authenticator](std::string_view application, std::string_view stream_name) {
            authenticator.on_viewer_attached(application, stream_name);
        };
    services.viewer_detached_handler =
        [&authenticator](std::string_view application, std::string_view stream_name) {
            authenticator.on_viewer_detached(application, stream_name);
        };
    services.admit_connection = [&authenticator](std::string_view client_ip) {
        return authenticator.admit_connection(client_ip);
    };
    services.release_connection = [&authenticator](std::string_view client_ip) {
        authenticator.release_connection(client_ip);
    };
    rtmp_server::io::io_uring::WorkerPool pool(config, stream_registry, stream_id_registry, std::move(services));

    // Open control plane: the management API is served without a bearer-token
    // check so hitting the web panel drops the operator straight into the
    // dashboard. Anyone who can reach this endpoint can create, disable and
    // disconnect streams.
    rtmp_server::control::ManagementApiOptions management_options{config.api_authentication_secret};
    management_options.require_authentication = false;
    rtmp_server::control::ManagementApi management_api(stream_manager, management_options);
    management_api.set_store(store.get());
    management_api.set_audit_log(&audit_log);
    management_api.set_metrics(&metrics);
    management_api.set_stream_id_registry(&stream_id_registry);
    management_api.set_live_state_provider([&stream_manager, &stream_registry, &stream_id_registry, &pool] {
        std::vector<rtmp_server::management::LiveState> states;
        const auto registrations = stream_registry.snapshot();
        for (const auto& application : stream_manager.list_applications()) {
            for (const auto& stream : stream_manager.list_streams(application.name)) {
                rtmp_server::management::LiveState state;
                state.application = stream.application;
                state.name = stream.name;
                state.is_live = std::ranges::any_of(registrations, [&stream](const auto& registration) {
                    return registration.app == stream.application && registration.stream_key == stream.name;
                });
                if (state.is_live) {
                    if (auto id = stream_id_registry.find(stream.application, stream.name)) {
                        state.viewer_count = pool.subscriber_count(*id);
                    }
                }
                states.push_back(std::move(state));
            }
        }
        return states;
    });

    rtmp_server::control::HttpServer api_server(
        {config.api_bind_address, config.api_port, 256, 4, 1024, 16 * 1024, 128 * 1024});
    api_server.set_handler([&management_api](const rtmp_server::control::HttpRequest& request) {
        return management_api.handle(request);
    });
    if (!api_server.start()) {
        std::fprintf(stderr, "failed to bind management API on %s:%u\n", config.api_bind_address.c_str(),
                     static_cast<unsigned>(config.api_port));
        return EXIT_FAILURE;
    }

    // Every connection each worker's ring accepts is driven through an RTMP
    // handshake automatically (IoUringEventLoop::start_handshake, wired from
    // handle_accept_completion in src/io/io_uring/event_loop.cpp) before any
    // higher-level RTMP processing begins — see docs/rtmp-handshake.md.
    g_pool = &pool;
    install_signal_handlers();

    auto run_result = pool.run();
    g_pool = nullptr;
    api_server.stop();

    if (!run_result) {
        std::fprintf(stderr, "worker pool exited with error: %s\n",
                      run_result.error().message().c_str());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
