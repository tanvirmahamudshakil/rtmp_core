#include "rtmp_server/network/tcp_connection.hpp"

#include "rtmp_server/io/io_uring/event_loop.hpp"
#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::network {

using observability::LogLevel;

TcpConnection::TcpConnection(io::io_uring::IoUringEventLoop& loop, core::FileDescriptor fd,
                              std::uint64_t connection_id, std::uint64_t generation, std::string client_ip,
                              std::size_t max_write_queue_bytes, std::size_t max_write_queue_packets)
    : loop_(loop),
      fd_(std::move(fd)),
      connection_id_(connection_id),
      generation_(generation),
      client_ip_(std::move(client_ip)),
      max_write_queue_bytes_(max_write_queue_bytes),
      max_write_queue_packets_(max_write_queue_packets) {}

void TcpConnection::start_read() { loop_.submit_receive(shared_from_this()); }

void TcpConnection::async_write(core::SharedBuffer buffer) {
    bool should_submit = false;
    bool queue_overflow = false;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (state_.load() == ConnectionState::Closed || state_.load() == ConnectionState::Closing) {
            return;
        }
        const bool bytes_over =
            max_write_queue_bytes_ != 0 && buffer.size() > max_write_queue_bytes_ - std::min(
                queued_bytes_, max_write_queue_bytes_);
        const bool packets_over =
            max_write_queue_packets_ != 0 && write_queue_.size() >= max_write_queue_packets_;
        if (bytes_over || packets_over) {
            // Never discard one encoded RTMP buffer and keep the connection
            // alive: ChunkEncoder state has already advanced, so doing that
            // would desynchronise every later chunk. A bounded connection is
            // safer than an unbounded or protocol-corrupt slow viewer.
            queue_overflow = true;
        } else {
            queued_bytes_ += buffer.size();
            write_queue_.push_back(std::move(buffer));
            if (!send_in_flight_) {
                send_in_flight_ = true;
                should_submit = true;
            }
        }
    }
    if (queue_overflow) {
        const auto backlog = pending_write_backlog();
        RTMP_LOG(LogLevel::Warn, "tcp_connection", "write_queue_limit_exceeded",
                 {{"connection_id", std::to_string(connection_id_)},
                  {"queued_bytes", std::to_string(backlog.bytes)},
                  {"queued_packets", std::to_string(backlog.packets)}});
        close();
        return;
    }
    if (should_submit) {
        loop_.submit_send(shared_from_this());
    }
}

core::SharedBuffer TcpConnection::next_write_buffer() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (write_queue_.empty()) return core::SharedBuffer{};
    return write_queue_.front();
}

void TcpConnection::pop_write_buffer() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!write_queue_.empty()) {
        const auto remaining = write_queue_.front().size() - std::min(front_offset_, write_queue_.front().size());
        queued_bytes_ -= std::min(queued_bytes_, remaining);
        write_queue_.pop_front();
    }
    front_offset_ = 0;
}

void TcpConnection::close() {
    auto expected = state_.load();
    if (expected == ConnectionState::Closing || expected == ConnectionState::Closed) return;
    state_.store(ConnectionState::Closing);
    loop_.request_close(shared_from_this());
}

void TcpConnection::on_receive_completion(std::span<const std::byte> data) {
    if (receive_handler_) receive_handler_(data);
}

void TcpConnection::on_send_completion(std::size_t bytes_sent, bool success) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (success && !write_queue_.empty()) {
        // A send completion reports the bytes the kernel actually accepted,
        // which may be fewer than requested. Advance into the front buffer
        // and only pop it once every byte has been transmitted; popping on
        // any success (the pre-Phase-7 behaviour) silently dropped the
        // remainder and desynchronised the peer's chunk decoder.
        const std::size_t accepted = std::min(bytes_sent, write_queue_.front().size() - front_offset_);
        front_offset_ += accepted;
        queued_bytes_ -= std::min(queued_bytes_, accepted);
        const std::size_t front_size = write_queue_.front().size();
        if (front_offset_ >= front_size) {
            write_queue_.pop_front();
            front_offset_ = 0;
        } else {
            partial_send_count_.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (!success) {
        // Failed send: the connection is being torn down by the event loop;
        // leave the queue intact for close() to discard.
        front_offset_ = 0;
    }
    send_in_flight_ = !write_queue_.empty();
    if (send_in_flight_) {
        // Re-submission of the next queued buffer is triggered by the event
        // loop after releasing this lock, to avoid recursive submission
        // from within the completion handler.
    }
}

void TcpConnection::on_peer_closed() {
    state_.store(ConnectionState::Closed);
    if (close_handler_) close_handler_();
}

} // namespace rtmp_server::network
