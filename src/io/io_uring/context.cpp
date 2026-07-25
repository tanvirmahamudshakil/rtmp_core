#include "rtmp_server/io/io_uring/context.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::io::io_uring {

using observability::LogLevel;

IoUringContext::IoUringContext(IoUringContext&& other) noexcept
    : ring_(other.ring_), capabilities_(other.capabilities_), valid_(std::exchange(other.valid_, false)) {}

IoUringContext& IoUringContext::operator=(IoUringContext&& other) noexcept {
    if (this != &other) {
        if (valid_) ::io_uring_queue_exit(&ring_);
        ring_ = other.ring_;
        capabilities_ = other.capabilities_;
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}

IoUringContext::~IoUringContext() {
    if (valid_) {
        ::io_uring_queue_exit(&ring_);
    }
}

core::Result<IoUringContext> IoUringContext::create(const IoUringContextOptions& options) {
    if (options.queue_depth == 0) {
        return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Configuration,
                            "io_uring queue_depth must be positive");
    }

    ::io_uring_params params{};
    if (options.enable_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = options.sqpoll_idle_ms;
    }

    ::io_uring ring{};
    int rc = ::io_uring_queue_init_params(options.queue_depth, &ring, &params);
    if (rc < 0) {
        // A total ring-init failure is fatal and must be reported clearly —
        // never silently fall back to blocking sockets/epoll (see
        // docs/rtmp_promot.md "io_uring Initialization").
        return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal,
                            std::string("io_uring_queue_init_params failed: ") + std::strerror(-rc));
    }

    IoUringCapabilities caps = detect_capabilities(ring);
    caps.sqpoll = options.enable_sqpoll;

    RTMP_LOG(LogLevel::Info, "io_uring", "ring_initialized",
             {{"queue_depth", std::to_string(options.queue_depth)},
              {"sqpoll", caps.sqpoll ? "true" : "false"}});
    RTMP_LOG(LogLevel::Info, "io_uring", "capabilities_detected", {});
    std::fputs(caps.to_report_string().c_str(), stdout);

    return IoUringContext(ring, caps);
}

} // namespace rtmp_server::io::io_uring
