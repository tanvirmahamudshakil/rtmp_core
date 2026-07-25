#pragma once

#include <liburing.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "rtmp_server/core/buffer.hpp"
#include "rtmp_server/core/config.hpp"
#include "rtmp_server/io/io_uring/context.hpp"
#include "rtmp_server/io/io_uring/operation_registry.hpp"
#include "rtmp_server/network/tcp_connection.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"
#include "rtmp_server/server/connection/connection_registry.hpp"

namespace rtmp_server::io::io_uring {

// Single-threaded io_uring event loop: owns one IoUringContext, one
// ConnectionRegistry, one receive BufferPool, and drives accept/receive/
// send/timeout/cancel completions to their connections.
//
// Phase 1 milestone runs exactly one of these on the main thread
// (docs/architecture.md section 8, Threading Model — worker_ring_count > 1
// is a later increment on top of this same class).
class IoUringEventLoop {
public:
    IoUringEventLoop(IoUringContext context, core::ServerConfig config);

    IoUringEventLoop(const IoUringEventLoop&) = delete;
    IoUringEventLoop& operator=(const IoUringEventLoop&) = delete;

    // Binds and listens on config().rtmp_bind_address:rtmp_port, then runs
    // the completion loop until stop() is called or a fatal error occurs.
    [[nodiscard]] core::Result<void> run();

    // Thread-safe: marks the loop for graceful shutdown. Actual teardown
    // (per docs/rtmp_promot.md "Graceful Shutdown") happens on the loop
    // thread on its next completion-processing iteration.
    void stop() noexcept { stopping_.store(true); }
    [[nodiscard]] bool is_stopping() const noexcept { return stopping_.load(); }

    [[nodiscard]] server::ConnectionRegistry& connections() noexcept { return connections_; }
    [[nodiscard]] const IoUringCapabilities& capabilities() const noexcept { return context_.capabilities(); }

    // Called by TcpConnection; not part of any external API.
    void submit_receive(std::shared_ptr<network::TcpConnection> connection);
    void submit_send(std::shared_ptr<network::TcpConnection> connection);
    void request_close(std::shared_ptr<network::TcpConnection> connection);

private:
    void submit_accept();
    void process_completion(std::uint64_t operation_id, int result, std::uint32_t flags);
    void handle_accept_completion(const OperationContext& op, int result);
    void handle_receive_completion(const OperationContext& op, int result);
    void handle_send_completion(const OperationContext& op, int result);
    void handle_timeout_completion(const OperationContext& op, int result);
    void close_connection(const std::shared_ptr<network::TcpConnection>& connection);
    void begin_shutdown();

    // Arms (or re-arms, cancelling any existing one first) the connection's
    // idle timeout. Called on accept and after every successful receive
    // (docs/rtmp_promot.md "Timeout Management" — idle connection timeout).
    void arm_idle_timeout(const std::shared_ptr<network::TcpConnection>& connection);

    // Creates a HandshakeSession for a freshly accepted connection, wires
    // its send/complete/fail handlers to the connection, and starts the
    // handshake timeout (docs/rtmp_promot.md Phase 2 "RTMP Handshake").
    // This is the connection-dispatch wiring point: the io_uring transport
    // (this file) still never has protocol *parsing* logic of its own, it
    // only owns the HandshakeSession's lifetime and forwards bytes/timeouts
    // to it, exactly as it already forwards bytes to TcpConnection's
    // receive_handler_.
    void start_handshake(const std::shared_ptr<network::TcpConnection>& connection);

    // Arms (or re-arms) the connection's handshake timeout, tracked
    // separately from the idle timeout since only one of the two purposes
    // is ever active at a time (handshake in progress vs. post-handshake).
    void arm_handshake_timeout(const std::shared_ptr<network::TcpConnection>& connection);

    // Drops the HandshakeSession and cancels any outstanding handshake
    // timeout for a connection. Called on handshake completion/failure and
    // on connection close, so a HandshakeSession never outlives its
    // connection.
    void cleanup_handshake_state(std::uint64_t connection_id);

    // Submits IORING_OP_ASYNC_CANCEL targeting `target_operation_id`. Fire
    // and forget: the cancellation's own completion (a Cancel-type op) is
    // processed and discarded; the *target* operation's own completion
    // (arriving separately, with -ECANCELED) is what callers actually react
    // to (docs/rtmp_promot.md "Async Cancellation").
    void cancel_operation(std::uint64_t target_operation_id);

    // Cancels every in-flight operation (receive/send/timeout) belonging to
    // a connection — used during close() / shutdown
    // (docs/rtmp_promot.md "Connection shutdown sequence").
    void cancel_all_operations_for_connection(std::uint64_t connection_id);

    IoUringContext context_;
    core::ServerConfig config_;
    core::FileDescriptor listener_;
    core::BufferPool receive_pool_;
    OperationRegistry operations_;
    std::mutex receive_buffers_mutex_;
    std::unordered_map<std::uint64_t, std::unique_ptr<core::ByteBuffer>> receive_buffers_;
    std::mutex timeout_specs_mutex_;
    std::unordered_map<std::uint64_t, std::unique_ptr<__kernel_timespec>> timeout_specs_;
    std::mutex handshake_mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<protocol::handshake::HandshakeSession>>
        handshake_sessions_;
    std::unordered_map<std::uint64_t, std::uint64_t> handshake_timeout_operation_ids_;
    server::ConnectionRegistry connections_;
    std::atomic<bool> stopping_{false};
    bool shutdown_initiated_ = false;
    std::chrono::steady_clock::time_point shutdown_deadline_{};
    std::uint64_t next_connection_id_ = 1;
    std::uint64_t next_generation_ = 1;
    std::uint64_t pending_accept_operation_id_ = 0;

    // Not a spec-required config key (docs/rtmp_promot.md's Configuration
    // section doesn't list one) — a fixed upper bound on graceful shutdown
    // so a stuck cancellation can't hang the process forever, per
    // "Apply a configurable shutdown deadline". Revisit as a config field if
    // production experience shows it needs tuning.
    static constexpr std::chrono::milliseconds kShutdownDeadline{10000};
};

} // namespace rtmp_server::io::io_uring
