#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "rtmp_server/core/buffer.hpp"
#include "rtmp_server/core/clock.hpp"
#include "rtmp_server/core/error.hpp"

namespace rtmp_server::protocol::handshake {

// RTMP "simple handshake" byte layout (docs/rtmp-handshake.md has the full
// write-up). All sizes are fixed by the protocol, not configurable.
inline constexpr std::uint8_t kRtmpVersion = 0x03;
inline constexpr std::size_t kC0Size = 1;
inline constexpr std::size_t kTimeFieldSize = 4;
inline constexpr std::size_t kZeroFieldSize = 4;
inline constexpr std::size_t kRandomEchoSize = 1528;
inline constexpr std::size_t kHandshakeChunkSize = kTimeFieldSize + kZeroFieldSize + kRandomEchoSize; // C1/C2/S1/S2
inline constexpr std::size_t kMaxHandshakeBytes = kC0Size + kHandshakeChunkSize + kHandshakeChunkSize; // C0+C1+C2

// Explicit RTMP handshake state machine (docs/rtmp_promot.md "RTMP Simple
// Handshake"). Pure protocol logic: no sockets, no io_uring — driven purely
// by byte chunks handed in via on_bytes_received(), which may be fragmented
// arbitrarily by the transport layer (docs/architecture.md "Architectural
// Separation").
enum class HandshakeState : std::uint8_t {
    WaitingForC0,
    WaitingForC1,
    SendingS0S1S2,
    WaitingForC2,
    Completed,
    Failed,
    TimedOut,
};

// Drives one connection's RTMP handshake. Owned by whatever layer wires a
// transport's receive callback to on_bytes_received() and its send handler
// to the transport's async_write() (see apps/rtmp_server and
// src/io/io_uring/event_loop.cpp for the current wiring point). Never
// touches a socket or io_uring directly.
class HandshakeSession {
public:
    // Buffer handed to the caller's send handler for transmission — already
    // ordered (S0+S1+S2 concatenated into one buffer so the transport layer
    // cannot interleave anything ahead of it on this connection).
    using SendHandler = std::function<void(core::SharedBuffer)>;
    using CompleteHandler = std::function<void()>;
    using FailHandler = std::function<void(core::Error)>;

    explicit HandshakeSession(core::MonotonicClock::time_point start_time = core::monotonic_now());

    void set_send_handler(SendHandler handler) { send_handler_ = std::move(handler); }
    void set_complete_handler(CompleteHandler handler) { complete_handler_ = std::move(handler); }
    void set_fail_handler(FailHandler handler) { fail_handler_ = std::move(handler); }

    // Feeds a fragment of bytes read off the wire, in order. May be called
    // any number of times with any chunk sizes, including a single byte at a
    // time or multiple RTMP messages' worth at once. Internally drives state
    // transitions and, at the appropriate points, invokes the send/complete/
    // fail handlers synchronously. No-op once Completed/Failed/TimedOut.
    void on_bytes_received(std::span<const std::byte> data);

    // Called by the timeout-owning layer when the handshake did not
    // complete within the configured deadline. No-op if already terminal.
    void on_timeout();

    [[nodiscard]] HandshakeState state() const noexcept { return state_; }
    [[nodiscard]] bool is_terminal() const noexcept {
        return state_ == HandshakeState::Completed || state_ == HandshakeState::Failed ||
               state_ == HandshakeState::TimedOut;
    }

    // Bytes that arrived after C2 completed in the same on_bytes_received()
    // call (e.g. the client's `connect` command pipelined onto the same TCP
    // write as C2 — real clients, including OBS, routinely do this). Only
    // meaningful once state() == Completed; empty otherwise. The caller
    // (session-owner layer, e.g. IoUringEventLoop::start_handshake) must
    // read this immediately inside/after the complete handler and feed it
    // into ChunkDecoder before installing the post-handshake receive
    // handler — otherwise these bytes would be silently lost, since
    // HandshakeSession itself no-ops on further on_bytes_received() calls
    // once terminal (docs/production-gap-analysis.md item #3).
    [[nodiscard]] std::vector<std::byte> take_trailing_bytes() noexcept { return std::move(buffer_); }

private:
    void fail(core::ErrorCode code, core::ErrorCategory category, std::string_view message);
    void try_consume_c0();
    void try_consume_c1();
    void try_consume_c2();

    HandshakeState state_ = HandshakeState::WaitingForC0;
    std::vector<std::byte> buffer_;
    core::MonotonicClock::time_point start_time_;

    SendHandler send_handler_;
    CompleteHandler complete_handler_;
    FailHandler fail_handler_;
};

} // namespace rtmp_server::protocol::handshake
