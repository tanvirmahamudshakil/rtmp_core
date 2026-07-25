#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "rtmp_server/core/buffer.hpp"

namespace rtmp_server::network {

// The only interface the protocol/media/server layers are allowed to depend
// on for I/O (see docs/architecture.md section 2, "Architectural
// Separation"). Concrete implementations (io_uring-backed) live in
// src/network and src/io/io_uring; nothing above this interface may include
// <liburing.h>.
class IAsyncTransport {
public:
    using ReceiveHandler = std::function<void(std::span<const std::byte>)>;
    using CloseHandler = std::function<void()>;

    virtual ~IAsyncTransport() = default;

    // Registers the callback invoked with each received chunk and begins
    // issuing receive operations. May deliver zero or more protocol
    // messages per call — see docs/rtmp_promot.md "Async Receive": one
    // receive does not equal one RTMP message.
    virtual void set_receive_handler(ReceiveHandler handler) = 0;
    virtual void set_close_handler(CloseHandler handler) = 0;

    virtual void start_read() = 0;

    // Queues `buffer` for ordered, in-sequence transmission. Must never let
    // a later call overtake an earlier one on the wire.
    virtual void async_write(core::SharedBuffer buffer) = 0;

    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
};

} // namespace rtmp_server::network
