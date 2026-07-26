#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "rtmp_server/core/config.hpp"
#include "rtmp_server/io/io_uring/worker_pool.hpp"
#include "rtmp_server/observability/logger.hpp"
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

    // Process-wide RTMP stream identity tables (Phase 1), shared by
    // reference across every WorkerPool worker so a publish/playback name
    // resolves to the same StreamId regardless of which worker's ring
    // accepted the connection (docs/v2_promot.md PHASE 4 task "avoid
    // cross-worker access to mutable connection state" — these two are the
    // one deliberate, mutex-guarded exception, by design; see
    // IoUringEventLoop's class doc).
    rtmp_server::protocol::commands::StreamRegistry stream_registry;
    rtmp_server::protocol::commands::StreamIdRegistry stream_id_registry;

    // Every connection each worker's ring accepts is driven through an RTMP
    // handshake automatically (IoUringEventLoop::start_handshake, wired from
    // handle_accept_completion in src/io/io_uring/event_loop.cpp) before any
    // higher-level RTMP processing begins — see docs/rtmp-handshake.md.
    rtmp_server::io::io_uring::WorkerPool pool(config_result.value(), stream_registry, stream_id_registry);
    g_pool = &pool;
    install_signal_handlers();

    auto run_result = pool.run();
    g_pool = nullptr;

    if (!run_result) {
        std::fprintf(stderr, "worker pool exited with error: %s\n",
                      run_result.error().message().c_str());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
