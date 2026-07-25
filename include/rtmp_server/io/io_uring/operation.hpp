#pragma once

#include <cstdint>
#include <memory>

namespace rtmp_server::network {
class TcpConnection;
}

namespace rtmp_server::io::io_uring {

enum class OperationType : std::uint8_t {
    Accept,
    Receive,
    Send,
    Timeout,
    Cancel,
    Close,
    FileWrite,
    Wakeup,
    Shutdown,
};

// Every timeout must carry its purpose (docs/rtmp_promot.md "Timeout
// Management") so a completion handler knows what to do on expiry without
// guessing from context.
enum class TimeoutPurpose : std::uint8_t {
    None,
    IdleConnection,
    Handshake,
    Authentication,
    PublisherInactivity,
    Write,
    ShutdownDeadline,
};

// Stable identity for a submitted SQE, referenced via io_uring_sqe_set_data64.
// Never identify an operation by fd alone — fds are reused by the OS as soon
// as a connection closes, and a late completion for a reused fd must not be
// mistaken for the new connection's operation (see docs/architecture.md
// section 6, docs/connection-lifecycle.md).
struct OperationContext {
    OperationType type;
    std::uint64_t operation_id;
    std::uint64_t connection_id;
    std::uint64_t generation;
    std::weak_ptr<network::TcpConnection> connection;
    TimeoutPurpose timeout_purpose = TimeoutPurpose::None;
};

} // namespace rtmp_server::io::io_uring
