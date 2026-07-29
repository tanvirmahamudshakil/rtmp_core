#pragma once

#include <liburing.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "rtmp_server/core/buffer.hpp"
#include "rtmp_server/core/config.hpp"
#include "rtmp_server/io/io_uring/context.hpp"
#include "rtmp_server/io/io_uring/cross_worker_router.hpp"
#include "rtmp_server/io/io_uring/operation_registry.hpp"
#include "rtmp_server/network/tcp_connection.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"
#include "rtmp_server/protocol/session/rtmp_connection_session.hpp"
#include "rtmp_server/server/connection/connection_registry.hpp"

namespace rtmp_server::io::io_uring {

// Process-owned services copied into every RTMP session a worker creates.
// The callbacks themselves may point at shared, internally synchronized
// management/authentication state and therefore must outlive the worker.
struct EventLoopServices {
    observability::Metrics* metrics = nullptr;
    protocol::commands::StreamKeyValidator key_validator;
    protocol::commands::StreamIdResolver stream_id_resolver;
    protocol::commands::PlaybackAuthorizer playback_authorizer;
    protocol::commands::ViewerLifecycleHandler viewer_attached_handler;
    protocol::commands::ViewerLifecycleHandler viewer_detached_handler;
    std::function<bool(std::string_view)> admit_connection;
    std::function<void(std::string_view)> release_connection;
};

// Single-threaded io_uring event loop: owns one IoUringContext, one
// ConnectionRegistry, one receive BufferPool, and drives accept/receive/
// send/timeout/cancel completions to their connections.
//
// Phase 4 (docs/v2_promot.md "Multi-core io_uring worker architecture"):
// a process runs `worker_ring_count` of these concurrently, one per
// std::thread, each with its own ring/connections/buffers/LiveFanout (see
// WorkerPool). `stream_registry`/`stream_id_registry` are therefore taken
// by reference — they are the one piece of state genuinely shared
// process-wide across workers (both are internally mutex-guarded, safe for
// concurrent access) — while LiveFanout stays a private, single-threaded-
// per-worker value member; cross-worker media fan-out goes through the
// optional CrossWorkerRouter instead of a shared LiveFanout (see
// cross_worker_router.hpp for why: GopCache/ViewerQueue have no locking of
// their own and must never be touched from more than one thread).
// A `router` of nullptr (and default worker_id 0 / reuseport false) is
// exactly the old Phase 1-3 single-worker shape.
class IoUringEventLoop {
public:
    IoUringEventLoop(IoUringContext context, core::ServerConfig config,
                      protocol::commands::StreamRegistry& stream_registry,
                      protocol::commands::StreamIdRegistry& stream_id_registry,
                      CrossWorkerRouter::WorkerId worker_id = 0, CrossWorkerRouter* router = nullptr,
                      bool enable_reuseport = false, EventLoopServices services = {});

    IoUringEventLoop(const IoUringEventLoop&) = delete;
    IoUringEventLoop& operator=(const IoUringEventLoop&) = delete;

    // Binds and listens on config().rtmp_bind_address:rtmp_port, then runs
    // the completion loop until stop() is called or a fatal error occurs.
    [[nodiscard]] core::Result<void> run();

    // Thread-safe: marks the loop for graceful shutdown. Actual teardown
    // (per docs/rtmp_promot.md "Graceful Shutdown") happens on the loop
    // thread on its next completion-processing iteration.
    void stop() noexcept;
    [[nodiscard]] bool is_stopping() const noexcept { return stopping_.load(); }

    // Phase 7: installs the shared metrics registry for this worker. Must be
    // called before run(); the registry must outlive the loop. Also forwards
    // it to this worker's LiveFanout so fan-out counters are recorded.
    void set_metrics(observability::Metrics* metrics) noexcept {
        metrics_ = metrics;
        live_fanout_.set_metrics(metrics);
    }

    [[nodiscard]] server::ConnectionRegistry& connections() noexcept { return connections_; }
    [[nodiscard]] const IoUringCapabilities& capabilities() const noexcept { return context_.capabilities(); }
    [[nodiscard]] std::size_t subscriber_count(protocol::commands::StreamId stream_id) const {
        return live_fanout_.subscriber_count(stream_id);
    }

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

    // Post-handshake wiring point (Phase 1, docs/v2_promot.md "PHASE 1"):
    // constructs the RtmpConnectionSession for a connection whose handshake
    // just completed, hands it any bytes the handshake buffered alongside
    // C2 (HandshakeSession::take_trailing_bytes — production-gap-analysis
    // item #3), and replaces the connection's receive handler so every
    // subsequent byte reaches the chunk/command pipeline instead of being
    // silently dropped (production-gap-analysis items #1/#2).
    void start_rtmp_session(const std::shared_ptr<network::TcpConnection>& connection,
                             std::vector<std::byte> trailing_bytes);
    void cleanup_rtmp_session_state(std::uint64_t connection_id);

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

    // Phase 4: registers a (re-armed) poll on router_->wake_fd(worker_id_)
    // so a cross-worker frame pushed by another worker wakes this worker's
    // own io_uring_submit_and_wait promptly instead of only being picked up
    // incidentally alongside unrelated local completions. No-op if
    // router_ is nullptr.
    void submit_router_poll();
    void handle_wakeup_completion(const OperationContext& op, int result);

    // Drains every frame router_->drain(worker_id_) currently has queued
    // and feeds each into this worker's *local* live_fanout_ as a replayed
    // frame (is_replayed=true — never re-forwarded, see live_fanout.hpp).
    void drain_router_frames();

    // Declared before context_ deliberately: members are destroyed in
    // reverse order, so the ring is torn down before these references are
    // released. SEND_ZC permits the kernel to retain source memory until a
    // notification CQE, including while shutdown is in progress.
    std::mutex zero_copy_buffers_mutex_;
    std::unordered_map<std::uint64_t, core::SharedBuffer> zero_copy_buffers_;
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

    // Process-wide RTMP stream identity tables (Phase 1), shared by
    // reference across every worker (see WorkerPool) so a publish/playback
    // name resolves to the same StreamId no matter which worker's ring
    // accepted the connection. Both are internally mutex-guarded.
    protocol::commands::StreamRegistry& stream_registry_;
    protocol::commands::StreamIdRegistry& stream_id_registry_;

    // Per-worker (Phase 4): each worker owns its own LiveFanout — its own
    // subscriber table and GopCache, touched only from this worker's
    // thread. Viewers subscribed on a *different* worker than the one
    // ingesting a given stream are reached via router_ instead (see class
    // doc comment).
    protocol::commands::LiveFanout live_fanout_;

    // Phase 7 observability (docs/v2_promot.md PHASE 7 "Metrics"). Non-owning
    // and optional; when set, this worker feeds the transport-level metrics
    // that only the io_uring layer can observe — io_uring_sq_full,
    // io_uring_cq_overflow, provided_buffer_exhaustion, partial_send_count,
    // connection_timeouts and connections_per_worker — plus forwards the
    // registry to live_fanout_ so fan-out metrics are attributed too.
    // set_metrics() must be called before run(); it is not safe to change
    // once the worker thread is running.
    observability::Metrics* metrics_ = nullptr;
    CrossWorkerRouter::WorkerId worker_id_;
    CrossWorkerRouter* router_;
    int router_wake_fd_ = -1;
    bool enable_reuseport_;
    bool zero_copy_send_enabled_ = false;
    bool zero_copy_fallback_logged_ = false;
    EventLoopServices services_;
    std::uint64_t pending_router_poll_operation_id_ = 0;
    std::mutex rtmp_sessions_mutex_;
    std::unordered_map<std::uint64_t, std::unique_ptr<protocol::session::RtmpConnectionSession>>
        rtmp_sessions_;
    std::atomic<bool> stopping_{false};
    bool shutdown_initiated_ = false;
    std::chrono::steady_clock::time_point shutdown_deadline_{};
    // Connection IDs are shared with the process-wide StreamRegistry, so a
    // worker-local sequence alone is not unique. The top byte identifies the
    // worker; the remaining 56 bits are this worker's monotonically
    // increasing sequence.
    std::uint64_t next_connection_id_ = 1;
    std::uint64_t next_generation_ = 1;
    std::uint64_t pending_accept_operation_id_ = 0;
    sockaddr_storage accept_address_{};
    socklen_t accept_address_length_ = sizeof(accept_address_);

    // Not a spec-required config key (docs/rtmp_promot.md's Configuration
    // section doesn't list one) — a fixed upper bound on graceful shutdown
    // so a stuck cancellation can't hang the process forever, per
    // "Apply a configurable shutdown deadline". Revisit as a config field if
    // production experience shows it needs tuning.
    static constexpr std::chrono::milliseconds kShutdownDeadline{10000};
    // Zero-copy setup/completion overhead is counterproductive for tiny
    // command/audio packets. Large video frames are where avoiding the
    // kernel copy pays for itself.
    static constexpr std::size_t kZeroCopySendMinimumBytes = 16 * 1024;
};

} // namespace rtmp_server::io::io_uring
