#include "rtmp_server/network/tcp_connection.hpp"

#include "rtmp_server/io/io_uring/event_loop.hpp"
#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::network {

using observability::LogLevel;

TcpConnection::TcpConnection(io::io_uring::IoUringEventLoop& loop, core::FileDescriptor fd,
                              std::uint64_t connection_id, std::uint64_t generation)
    : loop_(loop), fd_(std::move(fd)), connection_id_(connection_id), generation_(generation) {}

void TcpConnection::start_read() { loop_.submit_receive(shared_from_this()); }

void TcpConnection::async_write(core::SharedBuffer buffer) {
    bool should_submit = false;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (state_.load() == ConnectionState::Closed || state_.load() == ConnectionState::Closing) {
            return;
        }
        write_queue_.push_back(std::move(buffer));
        if (!send_in_flight_) {
            send_in_flight_ = true;
            should_submit = true;
        }
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
    if (!write_queue_.empty()) write_queue_.pop_front();
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
    (void)bytes_sent;
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (success) {
        if (!write_queue_.empty()) write_queue_.pop_front();
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
