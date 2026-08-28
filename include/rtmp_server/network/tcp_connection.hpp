#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

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
                  std::uint64_t connection_id, std::uint64_t generation, std::string client_ip = {},
                  std::size_t max_write_queue_bytes = 16 * 1024 * 1024,
                  std::size_t max_write_queue_packets = 2048);

    void set_receive_handler(ReceiveHandler handler) override { receive_handler_ = std::move(handler); }
    void set_close_handler(CloseHandler handler) override { close_handler_ = std::move(handler); }

    void start_read() override;
    void async_write(core::SharedBuffer buffer) override;
    void close() override;
    [[nodiscard]] bool is_open() const noexcept override { return state_ != ConnectionState::Closed; }

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] std::uint64_t connection_id() const noexcept { return connection_id_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const std::string& client_ip() const noexcept { return client_ip_; }
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

    // Total bytes currently queued but not yet confirmed sent. Both accessors
    // are O(1): this is sampled for every media frame of every viewer, so
    // walking the deque here turns one slow socket into a CPU amplification
    // problem under high fan-out.
    [[nodiscard]] std::size_t pending_write_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return queued_bytes_;
    }

    [[nodiscard]] std::size_t pending_write_packets() const noexcept {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return write_queue_.size();
    }

    struct WriteBacklog {
        std::size_t bytes = 0;
        std::size_t packets = 0;
    };
    // Hot playback path needs both values for every viewer/frame. Snapshot
    // them under one mutex acquisition instead of calling both accessors.
    [[nodiscard]] WriteBacklog pending_write_backlog() const noexcept {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return WriteBacklog{queued_bytes_, write_queue_.size()};
    }

    // Pops the next queued buffer for submission, or an empty SharedBuffer
    // if the queue is empty. The event loop drives one send at a time per
    // connection to preserve RTMP message/chunk order.
    [[nodiscard]] core::SharedBuffer next_write_buffer();
    void pop_write_buffer();

    // Bytes of the front buffer already confirmed sent by earlier partial
    // send completions. submit_send() must transmit only
    // next_write_buffer().view().subspan(next_write_offset()); sending from
    // offset 0 again would retransmit the already-delivered prefix and
    // corrupt the RTMP chunk stream.
    //
    // This exists because a successful io_uring send completion reports how
    // many bytes were accepted, which may be FEWER than requested
    // (docs/v2_promot.md 3.4: "A successful send completion does not
    // automatically mean the entire requested buffer was transmitted").
    // Before Phase 7 on_send_completion() ignored bytes_sent and popped the
    // whole front buffer on any success, silently discarding the unsent
    // remainder — see docs/phase-7-report.md "Problems confirmed".
    [[nodiscard]] std::size_t next_write_offset() const noexcept {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return front_offset_;
    }

    // Number of send completions that transmitted fewer bytes than
    // requested. Feeds the Phase 7 `partial_send_count` metric; read by the
    // owning event loop, which is the only thread that resets nothing here
    // (this is monotonic).
    [[nodiscard]] std::uint64_t partial_send_count() const noexcept {
        return partial_send_count_.load(std::memory_order_relaxed);
    }

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
    std::string client_ip_;
    std::atomic<ConnectionState> state_{ConnectionState::Accepted};

    ReceiveHandler receive_handler_;
    CloseHandler close_handler_;

    mutable std::mutex write_mutex_;
    std::deque<core::SharedBuffer> write_queue_;
    std::size_t front_offset_ = 0; // bytes of write_queue_.front() already sent
    std::size_t queued_bytes_ = 0; // unsent bytes across the entire deque
    std::size_t max_write_queue_bytes_;
    std::size_t max_write_queue_packets_;
    bool send_in_flight_ = false;
    std::atomic<std::uint64_t> partial_send_count_{0};

    std::uint64_t idle_timeout_operation_id_ = 0;
};

} // namespace rtmp_server::network
