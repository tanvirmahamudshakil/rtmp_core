#pragma once

#include <cstdint>
#include <string>

struct io_uring;

namespace rtmp_server::io::io_uring {

// Detected feature set for the running kernel's io_uring, probed once at
// startup (docs/rtmp_promot.md "io_uring Capability Detection"). Advanced
// feature absence must never crash the server or trigger an epoll fallback
// — callers branch on these flags to pick the correct io_uring-native path.
struct IoUringCapabilities {
    bool multishot_accept = false;
    bool multishot_recv = false;
    bool provided_buffers = false;
    bool registered_buffers = false;
    bool async_cancel = false;
    bool linked_timeout = false;
    bool send_zero_copy = false;
    bool sqpoll = false;
    bool cooperative_task_run = false;
    bool single_issuer = false;

    [[nodiscard]] std::string to_report_string() const;
};

// Probes a scratch io_uring instance for supported opcodes/setup flags.
// Must be called after io_uring_queue_init on a real ring, since capability
// support depends on kernel version, not just liburing version.
[[nodiscard]] IoUringCapabilities detect_capabilities(::io_uring& ring);

} // namespace rtmp_server::io::io_uring
