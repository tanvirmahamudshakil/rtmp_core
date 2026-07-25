#pragma once

#include <liburing.h>

#include <cstdint>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/io/io_uring/capabilities.hpp"

namespace rtmp_server::io::io_uring {

struct IoUringContextOptions {
    std::uint32_t queue_depth = 1024;
    bool enable_sqpoll = false;
    std::uint32_t sqpoll_idle_ms = 1000;
};

// Owns a single io_uring instance. Validates queue depth, initializes the
// ring, detects capabilities, and logs a startup report — per
// docs/rtmp_promot.md "io_uring Initialization". One IoUringContext per
// worker ring (docs/architecture.md section 8, Threading Model).
class IoUringContext {
public:
    IoUringContext(const IoUringContext&) = delete;
    IoUringContext& operator=(const IoUringContext&) = delete;

    // Move-only: exactly one IoUringContext owns a given ring_ at a time,
    // needed so Result<IoUringContext> can return it by value from create().
    IoUringContext(IoUringContext&& other) noexcept;
    IoUringContext& operator=(IoUringContext&& other) noexcept;

    ~IoUringContext();

    // Fails clearly (returns an Error) rather than silently falling back to
    // blocking sockets if the ring cannot be created at all. Advanced
    // feature absence (e.g. no multishot accept) is not fatal — only a
    // total ring-init failure is.
    [[nodiscard]] static core::Result<IoUringContext> create(const IoUringContextOptions& options);

    [[nodiscard]] ::io_uring& ring() noexcept { return ring_; }
    [[nodiscard]] const IoUringCapabilities& capabilities() const noexcept { return capabilities_; }

private:
    explicit IoUringContext(::io_uring ring, IoUringCapabilities capabilities)
        : ring_(ring), capabilities_(capabilities), valid_(true) {}

    ::io_uring ring_{};
    IoUringCapabilities capabilities_;
    bool valid_ = false;
};

} // namespace rtmp_server::io::io_uring
