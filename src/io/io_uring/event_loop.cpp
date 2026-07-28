#include "rtmp_server/io/io_uring/event_loop.hpp"

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::io::io_uring {

using observability::LogLevel;
using network::ConnectionState;
using network::TcpConnection;

IoUringEventLoop::IoUringEventLoop(IoUringContext context, core::ServerConfig config,
                                    protocol::commands::StreamRegistry& stream_registry,
                                    protocol::commands::StreamIdRegistry& stream_id_registry,
                                    CrossWorkerRouter::WorkerId worker_id, CrossWorkerRouter* router,
                                    bool enable_reuseport, EventLoopServices services)
    : context_(std::move(context)),
      config_(std::move(config)),
      receive_pool_(config_.provided_buffer_count, config_.provided_buffer_size),
      stream_registry_(stream_registry),
      stream_id_registry_(stream_id_registry),
      live_fanout_(protocol::commands::GopLimits{config_.gop_cache_max_bytes, config_.gop_cache_max_packets,
                                                  config_.gop_cache_max_duration},
                   // The connection-local ViewerQueue below sees the real
                   // TcpConnection backlog and owns slow-viewer decisions.
                   // A second transport-blind queue here would only count
                   // every delivered frame forever and eventually evict
                   // healthy viewers, so its synthetic caps are disabled in
                   // the production transport.
                   protocol::commands::QueueLimits{/*max_bytes=*/0, /*max_packets=*/0}),
      worker_id_(worker_id),
      router_(router),
      router_wake_fd_(router != nullptr ? router->wake_fd(worker_id) : -1),
      enable_reuseport_(enable_reuseport),
      services_(std::move(services)) {
    set_metrics(services_.metrics);
    if (router_ != nullptr) {
        // Lightweight, non-reentrant hooks only — never call back into
        // LiveFanout from inside them (see live_fanout.hpp's dispatch_locked
        // comment: the subscription hook can run while state.mutex is held).
        live_fanout_.set_forward_hook([this](protocol::commands::StreamId stream_id,
                                              const protocol::commands::SharedMediaFrame& frame, bool is_video,
                                              bool is_audio, bool is_sticky) {
            router_->forward(worker_id_, stream_id, frame, is_video, is_audio, is_sticky);
        });
        live_fanout_.set_subscription_hook([this](protocol::commands::StreamId stream_id, int delta) {
            router_->note_subscription(worker_id_, stream_id, delta);
        });
        live_fanout_.set_stream_end_hook(
            [this](protocol::commands::StreamId stream_id) { router_->on_stream_end(stream_id); });
    }
}

void IoUringEventLoop::stop() noexcept {
    stopping_.store(true);

    // io_uring_submit_and_wait() may otherwise remain blocked when an idle
    // service receives SIGTERM. WorkerPool supplies a per-worker eventfd via
    // CrossWorkerRouter; writing it is async-signal-safe and completes the
    // already-armed poll so the loop observes stopping_ immediately.
    if (router_wake_fd_ >= 0) {
        const std::uint64_t one = 1;
        [[maybe_unused]] const auto written = ::write(router_wake_fd_, &one, sizeof(one));
    }
}

core::Result<void> IoUringEventLoop::run() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Network,
                            std::string("socket() failed: ") + std::strerror(errno));
    }
    listener_ = core::FileDescriptor(fd);

    int reuse = 1;
    ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (enable_reuseport_) {
        // Phase 4 accept strategy (docs/v2_promot.md): every worker binds
        // its own listener on the same port with SO_REUSEPORT so the kernel
        // load-balances new connections across workers, instead of a
        // central-accept-plus-dispatch design that would need an extra
        // per-connection inter-worker handoff queue.
        ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.rtmp_port);
    if (::inet_pton(AF_INET, config_.rtmp_bind_address.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Network,
                            std::string("bind() failed: ") + std::strerror(errno));
    }
    if (::listen(listener_.get(), SOMAXCONN) < 0) {
        return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Network,
                            std::string("listen() failed: ") + std::strerror(errno));
    }

    RTMP_LOG(LogLevel::Info, "event_loop", "listening",
             {{"address", config_.rtmp_bind_address}, {"port", std::to_string(config_.rtmp_port)}});

    submit_accept();
    if (router_ != nullptr) submit_router_poll();

    io_uring_cqe* cqes[64];
    while (true) {
        if (stopping_.load() && !shutdown_initiated_) {
            begin_shutdown();
        }

        if (shutdown_initiated_) {
            if (operations_.pending_count() == 0) {
                break;
            }
            if (std::chrono::steady_clock::now() >= shutdown_deadline_) {
                RTMP_LOG(LogLevel::Warn, "event_loop", "shutdown_deadline_exceeded",
                         {{"pending_operations", std::to_string(operations_.pending_count())}});
                break;
            }
        }

        int submitted = ::io_uring_submit_and_wait(&context_.ring(), 1);
        if (submitted < 0 && submitted != -EINTR) {
            RTMP_LOG(LogLevel::Error, "event_loop", "submit_failed",
                     {{"errno", std::to_string(-submitted)}});
        }

        unsigned count = ::io_uring_peek_batch_cqe(&context_.ring(), cqes, 64);
        for (unsigned i = 0; i < count; ++i) {
            io_uring_cqe* cqe = cqes[i];
            auto operation_id = ::io_uring_cqe_get_data64(cqe);
            process_completion(operation_id, cqe->res, cqe->flags);
            ::io_uring_cqe_seen(&context_.ring(), cqe);
        }
    }

    // Step 16 (docs/rtmp_promot.md "Graceful Shutdown"): ring destruction
    // happens via IoUringContext's destructor once this function returns
    // and the caller drops the event loop. Step 14 (flush logs):
    std::fflush(stdout);
    std::fflush(stderr);

    RTMP_LOG(LogLevel::Info, "event_loop", "stopped", {});
    return {};
}

void IoUringEventLoop::begin_shutdown() {
    // Steps 1-10 of docs/rtmp_promot.md "Graceful Shutdown". Recording
    // (steps 6-7), publisher/viewer notification beyond a plain close
    // (steps 4-5), and persistence/database (step 15) are not yet
    // applicable — those concepts don't exist until later phases.
    shutdown_initiated_ = true;
    shutdown_deadline_ = std::chrono::steady_clock::now() + kShutdownDeadline;

    RTMP_LOG(LogLevel::Info, "event_loop", "shutdown_begin",
             {{"active_connections", std::to_string(connections_.size())}});

    // Step 2: stop accepting — cancel the outstanding accept SQE so the
    // ring doesn't keep it alive indefinitely; handle_accept_completion
    // already refuses to resubmit once stopping_ is set.
    if (pending_accept_operation_id_ != 0) {
        cancel_operation(pending_accept_operation_id_);
    }
    if (pending_router_poll_operation_id_ != 0) {
        cancel_operation(pending_router_poll_operation_id_);
    }

    // Steps 4/5/8/9/10: close every connection, which cancels its pending
    // receive/send/timeout operations and removes it from the registry.
    for (auto& conn : connections_.snapshot()) {
        conn->close();
    }
}

void IoUringEventLoop::submit_accept() {
    io_uring_sqe* sqe = ::io_uring_get_sqe(&context_.ring());
    if (sqe == nullptr) {
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::IoUringSqFull);
        RTMP_LOG(LogLevel::Error, "event_loop", "sqe_exhausted_accept", {});
        return;
    }
    std::memset(&accept_address_, 0, sizeof(accept_address_));
    accept_address_length_ = sizeof(accept_address_);
    ::io_uring_prep_accept(sqe, listener_.get(), reinterpret_cast<sockaddr*>(&accept_address_),
                           &accept_address_length_, SOCK_NONBLOCK | SOCK_CLOEXEC);

    OperationContext ctx{OperationType::Accept, 0, 0, 0, {}, TimeoutPurpose::None};
    auto id = operations_.create(std::move(ctx));
    pending_accept_operation_id_ = id;
    ::io_uring_sqe_set_data64(sqe, id);
}

void IoUringEventLoop::submit_router_poll() {
    io_uring_sqe* sqe = ::io_uring_get_sqe(&context_.ring());
    if (sqe == nullptr) {
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::IoUringSqFull);
        RTMP_LOG(LogLevel::Error, "event_loop", "sqe_exhausted_router_poll", {});
        return;
    }
    ::io_uring_prep_poll_add(sqe, router_->wake_fd(worker_id_), POLLIN);

    OperationContext ctx{OperationType::Wakeup, 0, 0, 0, {}, TimeoutPurpose::None};
    auto id = operations_.create(std::move(ctx));
    pending_router_poll_operation_id_ = id;
    ::io_uring_sqe_set_data64(sqe, id);
}

void IoUringEventLoop::handle_wakeup_completion(const OperationContext& /*op*/, int result) {
    if (result >= 0) {
        // Drain the eventfd counter (best-effort: EAGAIN just means another
        // forward() raced in and re-signaled after our poll already fired,
        // which the next poll_add will catch) before draining frames, so a
        // frame pushed between the read and the re-arm below still wakes us
        // again rather than being missed.
        std::uint64_t discard = 0;
        while (::read(router_->wake_fd(worker_id_), &discard, sizeof(discard)) > 0) {
        }
        drain_router_frames();
    } else if (result != -ECANCELED) {
        RTMP_LOG(LogLevel::Warn, "event_loop", "router_poll_failed", {{"errno", std::to_string(-result)}});
    }

    if (!stopping_.load()) submit_router_poll();
}

void IoUringEventLoop::drain_router_frames() {
    for (auto& queued : router_->drain(worker_id_)) {
        switch (queued.kind) {
            case CrossWorkerRouter::FrameKind::Video:
                live_fanout_.on_video(queued.stream_id, queued.frame, /*is_replayed=*/true);
                break;
            case CrossWorkerRouter::FrameKind::Audio:
                live_fanout_.on_audio(queued.stream_id, queued.frame, /*is_replayed=*/true);
                break;
            case CrossWorkerRouter::FrameKind::Metadata:
                live_fanout_.on_metadata(queued.stream_id, queued.frame, /*is_replayed=*/true);
                break;
        }
    }
}

void IoUringEventLoop::submit_receive(std::shared_ptr<TcpConnection> connection) {
    if (!connection->is_open()) return;
    auto buffer = receive_pool_.acquire();
    if (!buffer) {
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::ProvidedBufferExhaustion);
        RTMP_LOG(LogLevel::Warn, "event_loop", "receive_pool_exhausted",
                 {{"connection_id", std::to_string(connection->connection_id())}});
        return;
    }

    io_uring_sqe* sqe = ::io_uring_get_sqe(&context_.ring());
    if (sqe == nullptr) {
        receive_pool_.release(std::move(buffer));
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::IoUringSqFull);
        RTMP_LOG(LogLevel::Error, "event_loop", "sqe_exhausted_recv", {});
        return;
    }

    auto span = buffer->writable_span();
    ::io_uring_prep_recv(sqe, connection->fd(), span.data(), span.size(), 0);

    OperationContext ctx{OperationType::Receive, 0, connection->connection_id(),
                          connection->generation(), connection, TimeoutPurpose::None};
    auto id = operations_.create(std::move(ctx));
    ::io_uring_sqe_set_data64(sqe, id);

    // Buffer ownership transfers to receive_buffers_ keyed by operation_id
    // until the completion is processed — it must not be released back to
    // receive_pool_ before then (docs/architecture.md section 7, Buffer
    // Ownership: "no buffer reuse before operation completion").
    {
        std::lock_guard<std::mutex> lock(receive_buffers_mutex_);
        receive_buffers_.emplace(id, std::move(buffer));
    }
}

void IoUringEventLoop::submit_send(std::shared_ptr<TcpConnection> connection) {
    if (!connection->is_open()) return;
    auto buffer = connection->next_write_buffer();
    if (buffer.empty()) return;

    io_uring_sqe* sqe = ::io_uring_get_sqe(&context_.ring());
    if (sqe == nullptr) {
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::IoUringSqFull);
        RTMP_LOG(LogLevel::Error, "event_loop", "sqe_exhausted_send", {});
        return;
    }

    // Resume from the offset a previous partial send reached, never from the
    // start of the buffer — see TcpConnection::next_write_offset().
    auto view = buffer.view();
    const std::size_t offset = connection->next_write_offset();
    if (offset >= view.size()) {
        connection->pop_write_buffer();
        return;
    }
    view = view.subspan(offset);
    ::io_uring_prep_send(sqe, connection->fd(), view.data(), view.size(), 0);

    OperationContext ctx{OperationType::Send, 0, connection->connection_id(),
                          connection->generation(), connection, TimeoutPurpose::None};
    auto id = operations_.create(std::move(ctx));
    ::io_uring_sqe_set_data64(sqe, id);
}

void IoUringEventLoop::arm_idle_timeout(const std::shared_ptr<TcpConnection>& connection) {
    if (config_.idle_timeout.count() <= 0) return;
    if (!connection->is_open()) return;

    if (auto existing = connection->idle_timeout_operation_id(); existing != 0) {
        cancel_operation(existing);
        connection->set_idle_timeout_operation_id(0);
    }

    io_uring_sqe* sqe = ::io_uring_get_sqe(&context_.ring());
    if (sqe == nullptr) {
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::IoUringSqFull);
        RTMP_LOG(LogLevel::Error, "event_loop", "sqe_exhausted_timeout", {});
        return;
    }

    // __kernel_timespec must outlive the SQE submission; ownership lives in
    // timeout_specs_ keyed by operation_id until the completion is
    // processed, mirroring receive_buffers_ (docs/architecture.md
    // section 7, Buffer Ownership).
    auto ts = std::make_unique<__kernel_timespec>();
    ts->tv_sec = config_.idle_timeout.count() / 1000;
    ts->tv_nsec = (config_.idle_timeout.count() % 1000) * 1'000'000;
    ::io_uring_prep_timeout(sqe, ts.get(), 0, 0);

    OperationContext ctx{OperationType::Timeout, 0, connection->connection_id(),
                          connection->generation(), connection, TimeoutPurpose::IdleConnection};
    auto id = operations_.create(std::move(ctx));
    ::io_uring_sqe_set_data64(sqe, id);
    connection->set_idle_timeout_operation_id(id);

    {
        std::lock_guard<std::mutex> lock(timeout_specs_mutex_);
        timeout_specs_.emplace(id, std::move(ts));
    }
}

void IoUringEventLoop::cancel_operation(std::uint64_t target_operation_id) {
    io_uring_sqe* sqe = ::io_uring_get_sqe(&context_.ring());
    if (sqe == nullptr) {
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::IoUringSqFull);
        RTMP_LOG(LogLevel::Error, "event_loop", "sqe_exhausted_cancel", {});
        return;
    }
    // io_uring_prep_cancel (not the newer _cancel64) for compatibility with
    // Ubuntu 22.04's default liburing version — it matches by the same
    // 64-bit value passed to io_uring_sqe_set_data64 on the target op,
    // reinterpreted as a pointer-sized key.
    ::io_uring_prep_cancel(sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(target_operation_id)), 0);

    OperationContext ctx{OperationType::Cancel, 0, 0, 0, {}, TimeoutPurpose::None};
    auto id = operations_.create(std::move(ctx));
    ::io_uring_sqe_set_data64(sqe, id);
}

void IoUringEventLoop::cancel_all_operations_for_connection(std::uint64_t connection_id) {
    for (auto operation_id : operations_.find_all_for_connection(connection_id)) {
        cancel_operation(operation_id);
    }
}

void IoUringEventLoop::request_close(std::shared_ptr<TcpConnection> connection) {
    close_connection(connection);
}

void IoUringEventLoop::close_connection(const std::shared_ptr<TcpConnection>& connection) {
    // Connection shutdown sequence (docs/rtmp_promot.md): mark closing,
    // cancel its pending timeout/receive/send operations, then drop it
    // from the registry. The registry is the strong owner (see
    // connection_registry.hpp), so remove() is what actually destroys the
    // TcpConnection and closes its fd, once no other shared_ptr (e.g. a
    // caller's local copy) keeps it alive a moment longer. Operation
    // contexts hold only weak_ptr, so any completion that arrives after
    // this point safely no-ops (lock() returns nullptr).
    connection->set_state(ConnectionState::Closing);
    cancel_all_operations_for_connection(connection->connection_id());
    cleanup_handshake_state(connection->connection_id());
    cleanup_rtmp_session_state(connection->connection_id());
    if (services_.release_connection) services_.release_connection(connection->client_ip());

    connection->set_state(ConnectionState::Closed);
    connections_.remove(connection->connection_id());
    connection->on_peer_closed();
    if (metrics_ != nullptr) {
        if (!services_.release_connection) metrics_->add(observability::MetricId::ActiveConnections, -1);
        metrics_->set_connections_for_worker(worker_id_, static_cast<std::int64_t>(connections_.size()));
    }
    RTMP_LOG(LogLevel::Info, "event_loop", "connection_closed",
             {{"connection_id", std::to_string(connection->connection_id())}});
}

void IoUringEventLoop::process_completion(std::uint64_t operation_id, int result, std::uint32_t flags) {
    (void)flags;
    auto maybe_ctx = operations_.take(operation_id);
    if (!maybe_ctx) {
        // Stale completion for an operation we already forgot about
        // (e.g. after a connection was destroyed) — expected, not fatal.
        return;
    }
    OperationContext ctx = std::move(*maybe_ctx);

    if (ctx.type == OperationType::Accept) {
        pending_accept_operation_id_ = 0;
    } else if (ctx.type == OperationType::Wakeup) {
        pending_router_poll_operation_id_ = 0;
    }

    switch (ctx.type) {
        case OperationType::Accept:
            handle_accept_completion(ctx, result);
            break;
        case OperationType::Receive:
            handle_receive_completion(ctx, result);
            break;
        case OperationType::Send:
            handle_send_completion(ctx, result);
            break;
        case OperationType::Timeout:
            handle_timeout_completion(ctx, result);
            break;
        case OperationType::Wakeup:
            handle_wakeup_completion(ctx, result);
            break;
        case OperationType::Cancel:
            // The cancel request's own completion — its result (0 on
            // successful cancel-request submission, -ENOENT if the target
            // already completed) is informational only. The target
            // operation's real completion (-ECANCELED) is handled where
            // that operation type is processed.
            break;
        default:
            break;
    }
}

void IoUringEventLoop::handle_accept_completion(const OperationContext& /*op*/, int result) {
    // One accept is outstanding per worker, so capture its peer address
    // before submit_accept() reuses the storage for the next SQE.
    std::string client_ip;
    if (result >= 0) {
        char address[INET6_ADDRSTRLEN]{};
        const void* source = nullptr;
        const int family = accept_address_.ss_family;
        if (family == AF_INET) {
            source = &reinterpret_cast<const sockaddr_in*>(&accept_address_)->sin_addr;
        } else if (family == AF_INET6) {
            source = &reinterpret_cast<const sockaddr_in6*>(&accept_address_)->sin6_addr;
        }
        if (source != nullptr && ::inet_ntop(family, source, address, sizeof(address)) != nullptr) {
            client_ip = address;
        }
    }

    if (!stopping_.load()) {
        submit_accept();
    }

    if (result < 0) {
        if (result != -ECANCELED) {
            RTMP_LOG(LogLevel::Warn, "event_loop", "accept_failed",
                     {{"errno", std::to_string(-result)}});
        }
        return;
    }

    if (connections_.size() >= config_.maximum_connections) {
        RTMP_LOG(LogLevel::Warn, "event_loop", "connection_limit_reached", {});
        ::close(result);
        return;
    }
    if (services_.admit_connection && !services_.admit_connection(client_ip)) {
        RTMP_LOG(LogLevel::Warn, "event_loop", "per_ip_connection_limit_reached",
                 {{"client_ip", client_ip}});
        ::close(result);
        return;
    }

    int client_fd = result;
    int nodelay = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int keepalive = 1;
    ::setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    constexpr std::uint64_t kWorkerIdShift = 56;
    constexpr std::uint64_t kLocalIdMask = (std::uint64_t{1} << kWorkerIdShift) - 1;
    const auto local_connection_id = next_connection_id_++ & kLocalIdMask;
    const auto connection_id = ((static_cast<std::uint64_t>(worker_id_) + 1) << kWorkerIdShift) |
                               local_connection_id;
    auto generation = next_generation_++;
    auto connection = std::make_shared<TcpConnection>(*this, core::FileDescriptor(client_fd),
                                                        connection_id, generation, client_ip);
    connections_.add(connection);
    if (metrics_ != nullptr) {
        if (!services_.admit_connection) metrics_->add(observability::MetricId::ActiveConnections, +1);
        // Bounded-cardinality per-worker series: worker count comes from
        // configuration, never from client behaviour.
        metrics_->set_connections_for_worker(worker_id_, static_cast<std::int64_t>(connections_.size()));
    }

    RTMP_LOG(LogLevel::Info, "event_loop", "connection_accepted",
             {{"connection_id", std::to_string(connection_id)}, {"client_ip", client_ip}});

    connection->set_state(ConnectionState::Handshaking);
    start_handshake(connection);
    connection->start_read();
    arm_handshake_timeout(connection);
}

void IoUringEventLoop::start_handshake(const std::shared_ptr<TcpConnection>& connection) {
    auto session = std::make_shared<protocol::handshake::HandshakeSession>();
    std::weak_ptr<TcpConnection> weak_connection = connection;

    session->set_send_handler([weak_connection](core::SharedBuffer buffer) {
        if (auto conn = weak_connection.lock()) conn->async_write(std::move(buffer));
    });

    session->set_complete_handler([this, weak_connection, session]() {
        auto conn = weak_connection.lock();
        if (!conn) return;
        RTMP_LOG(LogLevel::Info, "event_loop", "handshake_completed",
                 {{"connection_id", std::to_string(conn->connection_id())}});
        conn->set_state(ConnectionState::Connected);

        // Bytes the peer sent alongside C2 in the same TCP read (e.g. OBS
        // pipelining `connect` right after the handshake) live in the
        // HandshakeSession's buffer until we take them here — must happen
        // before cleanup_handshake_state() drops the session
        // (production-gap-analysis item #3).
        std::vector<std::byte> trailing = session->take_trailing_bytes();

        cleanup_handshake_state(conn->connection_id());
        // Wires ChunkDecoder/ChunkEncoder/CommandSession to this
        // connection's socket I/O and installs the post-handshake receive
        // handler (Phase 1 tasks 1-8) — replaces the handshake's receive
        // handler so bytes actually reach the RTMP protocol layer instead
        // of being silently discarded (production-gap-analysis items #1/#2).
        start_rtmp_session(conn, std::move(trailing));
        arm_idle_timeout(conn);
    });

    session->set_fail_handler([this, weak_connection](core::Error error) {
        auto conn = weak_connection.lock();
        if (!conn) return;
        RTMP_LOG(LogLevel::Warn, "event_loop", "handshake_failed",
                 {{"connection_id", std::to_string(conn->connection_id())},
                  {"error", error.message()}});
        cleanup_handshake_state(conn->connection_id());
        close_connection(conn);
    });

    connection->set_receive_handler([session](std::span<const std::byte> data) {
        session->on_bytes_received(data);
    });

    std::lock_guard<std::mutex> lock(handshake_mutex_);
    handshake_sessions_[connection->connection_id()] = std::move(session);
}

void IoUringEventLoop::arm_handshake_timeout(const std::shared_ptr<TcpConnection>& connection) {
    if (config_.handshake_timeout.count() <= 0) return;
    if (!connection->is_open()) return;

    auto connection_id = connection->connection_id();
    {
        std::lock_guard<std::mutex> lock(handshake_mutex_);
        auto it = handshake_timeout_operation_ids_.find(connection_id);
        if (it != handshake_timeout_operation_ids_.end()) {
            cancel_operation(it->second);
            handshake_timeout_operation_ids_.erase(it);
        }
    }

    io_uring_sqe* sqe = ::io_uring_get_sqe(&context_.ring());
    if (sqe == nullptr) {
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::IoUringSqFull);
        RTMP_LOG(LogLevel::Error, "event_loop", "sqe_exhausted_handshake_timeout", {});
        return;
    }

    auto ts = std::make_unique<__kernel_timespec>();
    ts->tv_sec = config_.handshake_timeout.count() / 1000;
    ts->tv_nsec = (config_.handshake_timeout.count() % 1000) * 1'000'000;
    ::io_uring_prep_timeout(sqe, ts.get(), 0, 0);

    OperationContext ctx{OperationType::Timeout, 0, connection_id, connection->generation(), connection,
                          TimeoutPurpose::Handshake};
    auto id = operations_.create(std::move(ctx));
    ::io_uring_sqe_set_data64(sqe, id);

    {
        std::lock_guard<std::mutex> lock(handshake_mutex_);
        handshake_timeout_operation_ids_[connection_id] = id;
    }
    {
        std::lock_guard<std::mutex> lock(timeout_specs_mutex_);
        timeout_specs_.emplace(id, std::move(ts));
    }
}

void IoUringEventLoop::start_rtmp_session(const std::shared_ptr<TcpConnection>& connection,
                                           std::vector<std::byte> trailing_bytes) {
    protocol::session::RtmpConnectionSession::Dependencies deps;
    deps.registry = &stream_registry_;
    deps.stream_id_registry = &stream_id_registry_;
    deps.live_fanout = &live_fanout_;
    deps.key_validator = services_.key_validator;
    deps.stream_id_resolver = services_.stream_id_resolver;
    deps.playback_authorizer = services_.playback_authorizer;
    deps.viewer_attached_handler = services_.viewer_attached_handler;
    deps.viewer_detached_handler = services_.viewer_detached_handler;
    deps.client_ip = connection->client_ip();
    deps.playback_queue_limits =
        protocol::commands::QueueLimits{config_.subscriber_queue_max_bytes, config_.subscriber_queue_max_packets};

    auto session = std::make_unique<protocol::session::RtmpConnectionSession>(
        connection->connection_id(), deps, config_.maximum_rtmp_message_size, config_.output_chunk_size);
    session->set_metrics(metrics_);

    std::weak_ptr<TcpConnection> weak_connection = connection;
    auto connection_id = connection->connection_id();

    // Encoded outgoing RTMP bytes (command responses, control messages,
    // playback media) reach the socket through TcpConnection::async_write,
    // which itself queues and honours partial-send semantics (Phase 1 task
    // 7 / Phase 2 concern for partial-send correctness itself).
    session->set_outgoing_handler([weak_connection](std::vector<std::byte> bytes) {
        if (auto conn = weak_connection.lock()) {
            conn->async_write(core::SharedBuffer::adopt(std::move(bytes)));
        }
    });

    // A malformed chunk stream / decode failure closes the connection
    // deterministically (Phase 1 task 11) rather than leaving it half-open.
    session->set_close_handler([this, weak_connection]() {
        if (auto conn = weak_connection.lock()) close_connection(conn);
    });

    // Playback backpressure (CommandSession::set_pending_bytes_provider)
    // reads this connection's actual queued-but-unsent byte count so a slow
    // viewer's queue growth is visible to the drop decision.
    session->set_pending_bytes_provider([weak_connection]() -> std::size_t {
        auto conn = weak_connection.lock();
        return conn ? conn->pending_write_bytes() : 0;
    });

    protocol::session::RtmpConnectionSession* raw_session = session.get();
    // Must run after every handler above is wired (start() may emit a Set
    // Chunk Size control message immediately) and before the receive
    // handler swap below, so nothing the peer sends can race ahead of it.
    raw_session->start();
    connection->set_receive_handler([raw_session](std::span<const std::byte> data) {
        raw_session->on_bytes_received(data);
    });

    {
        std::lock_guard<std::mutex> lock(rtmp_sessions_mutex_);
        rtmp_sessions_[connection_id] = std::move(session);
    }

    // Feed the C2-adjacent bytes (if any) through the exact same code path
    // as any other received bytes, now that the pipeline is fully wired.
    if (!trailing_bytes.empty()) {
        raw_session->on_bytes_received(trailing_bytes);
    }
}

void IoUringEventLoop::cleanup_rtmp_session_state(std::uint64_t connection_id) {
    std::unique_ptr<protocol::session::RtmpConnectionSession> session;
    {
        std::lock_guard<std::mutex> lock(rtmp_sessions_mutex_);
        auto it = rtmp_sessions_.find(connection_id);
        if (it != rtmp_sessions_.end()) {
            session = std::move(it->second);
            rtmp_sessions_.erase(it);
        }
    }
    // Deterministic teardown (Phase 1 task 12): drops any publisher/viewer
    // registration this connection held before the session object itself is
    // destroyed, regardless of what state the connection was in.
    if (session) session->on_connection_closed();
}

void IoUringEventLoop::cleanup_handshake_state(std::uint64_t connection_id) {
    std::uint64_t timeout_id = 0;
    {
        std::lock_guard<std::mutex> lock(handshake_mutex_);
        handshake_sessions_.erase(connection_id);
        auto it = handshake_timeout_operation_ids_.find(connection_id);
        if (it != handshake_timeout_operation_ids_.end()) {
            timeout_id = it->second;
            handshake_timeout_operation_ids_.erase(it);
        }
    }
    if (timeout_id != 0) cancel_operation(timeout_id);
}

void IoUringEventLoop::handle_receive_completion(const OperationContext& op, int result) {
    std::unique_ptr<core::ByteBuffer> buffer;
    {
        std::lock_guard<std::mutex> lock(receive_buffers_mutex_);
        auto it = receive_buffers_.find(op.operation_id);
        if (it != receive_buffers_.end()) {
            buffer = std::move(it->second);
            receive_buffers_.erase(it);
        }
    }

    auto connection = op.connection.lock();

    if (result <= 0) {
        if (buffer) receive_pool_.release(std::move(buffer));
        if (connection && result != -ECANCELED) {
            close_connection(connection);
        }
        return;
    }

    if (buffer && buffer->commit(static_cast<std::size_t>(result)) && connection &&
        connection->is_open()) {
        connection->on_receive_completion(buffer->readable_span());
    }
    if (buffer) receive_pool_.release(std::move(buffer));

    if (connection && connection->is_open()) {
        connection->start_read();

        bool handshaking = false;
        {
            std::lock_guard<std::mutex> lock(handshake_mutex_);
            handshaking = handshake_sessions_.contains(connection->connection_id());
        }
        if (handshaking) {
            arm_handshake_timeout(connection);
        } else {
            arm_idle_timeout(connection);
        }
    }
}

void IoUringEventLoop::handle_send_completion(const OperationContext& op, int result) {
    auto connection = op.connection.lock();
    if (!connection) return;

    bool success = result > 0;
    const std::uint64_t partials_before = connection->partial_send_count();
    connection->on_send_completion(success ? static_cast<std::size_t>(result) : 0, success);
    if (metrics_ != nullptr) {
        if (success) metrics_->increment(observability::MetricId::EgressBytesTotal,
                                         static_cast<std::uint64_t>(result));
        const std::uint64_t delta = connection->partial_send_count() - partials_before;
        if (delta > 0) metrics_->increment(observability::MetricId::PartialSendCount, delta);
    }

    if (connection->is_open() && connection->has_pending_write()) {
        submit_send(connection);
    }
}

void IoUringEventLoop::handle_timeout_completion(const OperationContext& op, int result) {
    {
        std::lock_guard<std::mutex> lock(timeout_specs_mutex_);
        timeout_specs_.erase(op.operation_id);
    }

    auto connection = op.connection.lock();
    if (!connection) return;

    if (connection->idle_timeout_operation_id() == op.operation_id) {
        connection->set_idle_timeout_operation_id(0);
    }

    std::shared_ptr<protocol::handshake::HandshakeSession> handshake_session;
    if (op.timeout_purpose == TimeoutPurpose::Handshake) {
        std::lock_guard<std::mutex> lock(handshake_mutex_);
        auto it = handshake_timeout_operation_ids_.find(connection->connection_id());
        if (it != handshake_timeout_operation_ids_.end() && it->second == op.operation_id) {
            handshake_timeout_operation_ids_.erase(it);
            auto session_it = handshake_sessions_.find(connection->connection_id());
            if (session_it != handshake_sessions_.end()) handshake_session = session_it->second;
        }
    }

    if (result == -ECANCELED) {
        // Expected: either re-armed after activity, or the connection is
        // closing — not an error (docs/rtmp_promot.md "Expected
        // cancellation results must not be logged as fatal errors").
        return;
    }

    // result == -ETIME (fired) or 0 (fired with count semantics on some
    // kernels) both mean the timeout period elapsed with no re-arm.
    if (op.timeout_purpose == TimeoutPurpose::IdleConnection && connection->is_open()) {
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::ConnectionTimeouts);
        RTMP_LOG(LogLevel::Info, "event_loop", "idle_timeout",
                 {{"connection_id", std::to_string(connection->connection_id())}});
        close_connection(connection);
    } else if (op.timeout_purpose == TimeoutPurpose::Handshake && handshake_session &&
               connection->is_open()) {
        if (metrics_ != nullptr) metrics_->increment(observability::MetricId::ConnectionTimeouts);
        RTMP_LOG(LogLevel::Warn, "event_loop", "handshake_timeout",
                 {{"connection_id", std::to_string(connection->connection_id())}});
        // Drives the session into TimedOut and invokes its fail handler,
        // which cleans up handshake state and closes the connection (wired
        // in start_handshake()).
        handshake_session->on_timeout();
    }
}

} // namespace rtmp_server::io::io_uring
