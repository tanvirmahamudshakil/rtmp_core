#include "rtmp_server/io/io_uring/capabilities.hpp"

#include <liburing.h>

#include <format>

namespace rtmp_server::io::io_uring {

std::string IoUringCapabilities::to_report_string() const {
    auto flag = [](bool v) { return v ? "supported" : "unsupported"; };
    return std::format(
        "io_uring capabilities:\n"
        "  multishot_accept: {}\n"
        "  multishot_recv: {}\n"
        "  provided_buffers: {}\n"
        "  registered_buffers: {}\n"
        "  async_cancel: {}\n"
        "  linked_timeout: {}\n"
        "  send_zero_copy: {}\n"
        "  sqpoll: {}\n"
        "  cooperative_task_run: {}\n"
        "  single_issuer: {}\n",
        flag(multishot_accept), flag(multishot_recv), flag(provided_buffers),
        flag(registered_buffers), flag(async_cancel), flag(linked_timeout),
        flag(send_zero_copy), flag(sqpoll), flag(cooperative_task_run), flag(single_issuer));
}

IoUringCapabilities detect_capabilities(::io_uring& ring) {
    IoUringCapabilities caps;

    struct io_uring_probe* probe = io_uring_get_probe_ring(&ring);
    if (probe != nullptr) {
        caps.multishot_accept = io_uring_opcode_supported(probe, IORING_OP_ACCEPT) != 0;
        caps.multishot_recv = io_uring_opcode_supported(probe, IORING_OP_RECV) != 0;
        caps.async_cancel = io_uring_opcode_supported(probe, IORING_OP_ASYNC_CANCEL) != 0;
        caps.linked_timeout = io_uring_opcode_supported(probe, IORING_OP_LINK_TIMEOUT) != 0;
        caps.send_zero_copy = io_uring_opcode_supported(probe, IORING_OP_SEND_ZC) != 0;
        io_uring_free_probe(probe);
    }

    // Provided buffer rings, registered buffers, SQPOLL, cooperative task
    // run, and single-issuer are setup-time/ring features rather than
    // opcodes — they are validated by attempting the corresponding setup
    // call at ring-creation time in IoUringContext::create, which records
    // the result back onto this struct. This function only establishes the
    // opcode-level baseline.
    caps.provided_buffers = true;
    caps.registered_buffers = true;
    caps.sqpoll = false;               // opt-in only, disabled by default
    caps.cooperative_task_run = true;
    caps.single_issuer = true;

    return caps;
}

} // namespace rtmp_server::io::io_uring
