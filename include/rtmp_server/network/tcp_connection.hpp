#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "rtmp_server/core/buffer.hpp"
#include "rtmp_server/core/file_descriptor.hpp"
#include "rtmp_server/network/async_transport.hpp"

namespace rtmp_server::io::io_uring {
class IoUringEventLoop;
}

namespace rtmp_server::network {

enum class ConnectionState : std::uint8_t {
    Accepted,
    Handshaking,
    Connected,
    Authenticating,
    Publishing,
    Playing,
    Closing,
    Closed,
    Failed,
};

// io_uring-backed IAsyncTransport for one accepted TCP socket. Owned by
// exactly one IoUringEventLoop (docs/architecture.md section 5); all
// mutation happens on that loop's thread except close(), which may be
// requested cross-thread and is applied by the owning loop.
class TcpConnection : public IAsyncTransport, public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(io::io_uring::IoUringEventLoop& loop, core::FileDescriptor fd,
                  std::uint64_t connection_id, std::uint64_t generation);

    void set_receive_handler(ReceiveHandler handler) override { receive_handler_ = std::move(handler); }
    void set_close_handler(CloseHandler handler) override { close_handler_ = std::move(handler); }

    void start_read() override;
    void async_write(core::SharedBuffer buffer) override;
    void close() override;
    [[nodiscard]] bool is_open() const noexcept override { return state_ != ConnectionState::Closed; }

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] std::uint64_t connection_id() const noexcept { return connection_id_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] ConnectionState state() const noexcept { return state_.load(); }
    void set_state(ConnectionState state) noexcept { state_.store(state); }

    // Called by the event loop on receive/send completions; not part of the
    // public IAsyncTransport surface.
    void on_receive_completion(std::span<const std::byte> data);
    void on_send_completion(std::size_t bytes_sent, bool success);
    void on_peer_closed();

    [[nodiscard]] bool has_pending_write() const noexcept {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return !write_queue_.empty();
    }

    // Total bytes currently queued but not yet confirmed sent — used by the
    // RTMP session layer (CommandSession::set_pending_bytes_provider) to
    // decide whether a slow playback viewer's next frame should be dropped
    // rather than grown into an unbounded backlog (Phase 1 wiring; the
    // queue itself becoming byte/packet bounded is a Phase 2/3 concern).
    [[nodiscard]] std::size_t pending_write_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(write_mutex_);
        std::size_t total = 0;
        for (const auto& buffer : write_queue_) total += buffer.size();
        return total;
    }

    // Pops the next queued buffer for submission, or an empty SharedBuffer
    // if the queue is empty. The event loop drives one send at a time per
    // connection to preserve RTMP message/chunk order.
    [[nodiscard]] core::SharedBuffer next_write_buffer();
    void pop_write_buffer();

    // Tracks the currently-armed idle-timeout operation so a new receive
    // can cancel-and-rearm it, and so close() can cancel it explicitly.
    // 0 means "no timeout currently armed".
    void set_idle_timeout_operation_id(std::uint64_t id) noexcept { idle_timeout_operation_id_ = id; }
    [[nodiscard]] std::uint64_t idle_timeout_operation_id() const noexcept {
        return idle_timeout_operation_id_;
    }

private:
    io::io_uring::IoUringEventLoop& loop_;
    core::FileDescriptor fd_;
    std::uint64_t connection_id_;
    std::uint64_t generation_;
    std::atomic<ConnectionState> state_{ConnectionState::Accepted};

    ReceiveHandler receive_handler_;
    CloseHandler close_handler_;

    mutable std::mutex write_mutex_;
    std::deque<core::SharedBuffer> write_queue_;
    bool send_in_flight_ = false;

    std::uint64_t idle_timeout_operation_id_ = 0;
};

} // namespace rtmp_server::network
