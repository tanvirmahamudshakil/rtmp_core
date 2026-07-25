#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "rtmp_server/core/config.hpp"
#include "rtmp_server/io/io_uring/context.hpp"
#include "rtmp_server/io/io_uring/event_loop.hpp"
#include "rtmp_server/observability/logger.hpp"

namespace {

rtmp_server::io::io_uring::IoUringEventLoop* g_loop = nullptr;

// Signal handlers must be async-signal-safe: only touch an atomic/flag and
// return. Actual teardown happens on the event-loop thread inside run(),
// per docs/rtmp_promot.md "Graceful Shutdown".
void handle_shutdown_signal(int /*signum*/) {
    if (g_loop != nullptr) g_loop->stop();
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

    rtmp_server::io::io_uring::IoUringContextOptions ring_options;
    ring_options.queue_depth = config_result.value().ring_queue_depth;
    ring_options.enable_sqpoll = config_result.value().enable_sqpoll;
    ring_options.sqpoll_idle_ms = config_result.value().sqpoll_idle_ms;

    auto context_result = rtmp_server::io::io_uring::IoUringContext::create(ring_options);
    if (!context_result) {
        std::fprintf(stderr, "failed to initialize io_uring: %s\n",
                      context_result.error().message().c_str());
        return EXIT_FAILURE;
    }

    // Every connection IoUringEventLoop accepts is driven through an RTMP
    // handshake automatically (IoUringEventLoop::start_handshake, wired from
    // handle_accept_completion in src/io/io_uring/event_loop.cpp) before any
    // higher-level RTMP processing begins — see docs/rtmp-handshake.md.
    rtmp_server::io::io_uring::IoUringEventLoop loop(std::move(context_result).value(),
                                                       config_result.value());
    g_loop = &loop;
    install_signal_handlers();

    auto run_result = loop.run();
    g_loop = nullptr;

    if (!run_result) {
        std::fprintf(stderr, "event loop exited with error: %s\n",
                      run_result.error().message().c_str());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
