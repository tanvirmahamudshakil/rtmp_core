#include "rtmp_server/io/io_uring/file_sink.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::io::io_uring {

using core::Error;
using core::ErrorCategory;
using core::ErrorCode;
using observability::LogField;
using observability::LogLevel;

core::Result<IoUringFileSink> IoUringFileSink::open(const std::string& path,
                                                    std::size_t max_inflight_bytes,
                                                    std::uint32_t queue_depth) {
    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return Error{ErrorCode::StorageUnavailable, ErrorCategory::Storage,
                     std::string("open failed: ") + std::strerror(errno)};
    }
    ::io_uring ring{};
    if (int rc = ::io_uring_queue_init(queue_depth, &ring, 0); rc < 0) {
        ::close(fd);
        return Error{ErrorCode::StorageUnavailable, ErrorCategory::Storage,
                     std::string("io_uring_queue_init failed: ") + std::strerror(-rc)};
    }
    return IoUringFileSink(ring, fd, max_inflight_bytes);
}

IoUringFileSink::IoUringFileSink(IoUringFileSink&& other) noexcept
    : ring_(other.ring_),
      fd_(other.fd_),
      max_inflight_bytes_(other.max_inflight_bytes_),
      write_offset_(other.write_offset_),
      inflight_bytes_(other.inflight_bytes_),
      next_id_(other.next_id_),
      inflight_(std::move(other.inflight_)),
      failed_(other.failed_),
      valid_(other.valid_) {
    other.valid_ = false;
    other.fd_ = -1;
}

IoUringFileSink& IoUringFileSink::operator=(IoUringFileSink&& other) noexcept {
    if (this != &other) {
        close_ring_and_fd();
        ring_ = other.ring_;
        fd_ = other.fd_;
        max_inflight_bytes_ = other.max_inflight_bytes_;
        write_offset_ = other.write_offset_;
        inflight_bytes_ = other.inflight_bytes_;
        next_id_ = other.next_id_;
        inflight_ = std::move(other.inflight_);
        failed_ = other.failed_;
        valid_ = other.valid_;
        other.valid_ = false;
        other.fd_ = -1;
    }
    return *this;
}

IoUringFileSink::~IoUringFileSink() { close_ring_and_fd(); }

void IoUringFileSink::close_ring_and_fd() noexcept {
    if (!valid_) return;
    // Best-effort drain so in-flight buffers are not freed while the kernel
    // may still read them.
    reap(false);
    ::io_uring_queue_exit(&ring_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    valid_ = false;
}

void IoUringFileSink::mark_failed(int error, const char* where) {
    if (failed_) return;
    failed_ = true;
    RTMP_LOG(LogLevel::Error, "file_sink", "write_failed",
             {LogField{"where", where}, LogField{"errno", std::to_string(error)}});
}

void IoUringFileSink::reap(bool wait) {
    // Reap every ready completion (and optionally block for at least one).
    bool first = wait;
    while (true) {
        ::io_uring_cqe* cqe = nullptr;
        int rc = 0;
        if (first) {
            rc = ::io_uring_wait_cqe(&ring_, &cqe);
            first = false;
        } else {
            rc = ::io_uring_peek_cqe(&ring_, &cqe);
        }
        if (rc < 0 || cqe == nullptr) break;

        const auto id = ::io_uring_cqe_get_data64(cqe);
        const int res = cqe->res;
        ::io_uring_cqe_seen(&ring_, cqe);

        // Locate the matching in-flight buffer (writes complete in submission
        // order on a regular file, but match by id to be safe).
        for (auto it = inflight_.begin(); it != inflight_.end(); ++it) {
            if (it->id == id) {
                if (res < 0) {
                    mark_failed(-res, "write_cqe");
                } else if (static_cast<std::size_t>(res) < it->buffer.size()) {
                    // Short write: the FLV file would be corrupt past this
                    // point. Treat as a disk failure rather than silently
                    // truncating a tag.
                    mark_failed(EIO, "short_write");
                }
                inflight_bytes_ -= it->buffer.size();
                inflight_.erase(it);
                break;
            }
        }
    }
}

core::Result<void> IoUringFileSink::append(std::span<const std::byte> data) {
    if (failed_ || fd_ < 0) {
        return Error{ErrorCode::StorageWriteFailed, ErrorCategory::Storage, "sink already failed"};
    }
    if (data.empty()) return {};

    // Make room if we are at/over the in-flight bound: block-drain completions.
    while (inflight_bytes_ + data.size() > max_inflight_bytes_ && !inflight_.empty()) {
        reap(true);
        if (failed_) {
            return Error{ErrorCode::StorageWriteFailed, ErrorCategory::Storage, "sink failed while draining"};
        }
    }

    Inflight node;
    node.id = next_id_++;
    node.buffer.assign(data.begin(), data.end());
    const std::uint64_t offset = write_offset_;
    write_offset_ += node.buffer.size();

    ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        // Ring full: drain then retry once.
        reap(true);
        sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            return Error{ErrorCode::ResourceExhausted, ErrorCategory::Storage, "sqe exhausted"};
        }
    }
    inflight_bytes_ += node.buffer.size();
    ::io_uring_prep_write(sqe, fd_, node.buffer.data(), static_cast<unsigned>(node.buffer.size()),
                          offset);
    ::io_uring_sqe_set_data64(sqe, node.id);
    inflight_.push_back(std::move(node));

    if (int rc = ::io_uring_submit(&ring_); rc < 0) {
        mark_failed(-rc, "submit");
        return Error{ErrorCode::StorageWriteFailed, ErrorCategory::Storage, "io_uring_submit failed"};
    }
    reap(false);
    return {};
}

core::Result<void> IoUringFileSink::patch(std::uint64_t offset, std::span<const std::byte> data) {
    if (failed_ || fd_ < 0) {
        return Error{ErrorCode::StorageWriteFailed, ErrorCategory::Storage, "sink already failed"};
    }
    // Patching the header placeholders must land before close and must not
    // overlap with still-in-flight appends, so drain first, then do one
    // synchronous io_uring write and wait for it.
    reap(true);
    while (!inflight_.empty()) reap(true);
    if (failed_) {
        return Error{ErrorCode::StorageWriteFailed, ErrorCategory::Storage, "sink failed before patch"};
    }

    ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        return Error{ErrorCode::ResourceExhausted, ErrorCategory::Storage, "sqe exhausted (patch)"};
    }
    const auto id = next_id_++;
    ::io_uring_prep_write(sqe, fd_, data.data(), static_cast<unsigned>(data.size()), offset);
    ::io_uring_sqe_set_data64(sqe, id);
    if (int rc = ::io_uring_submit(&ring_); rc < 0) {
        mark_failed(-rc, "submit_patch");
        return Error{ErrorCode::StorageWriteFailed, ErrorCategory::Storage, "submit patch failed"};
    }
    ::io_uring_cqe* cqe = nullptr;
    if (int rc = ::io_uring_wait_cqe(&ring_, &cqe); rc < 0 || cqe == nullptr) {
        mark_failed(rc < 0 ? -rc : EIO, "wait_patch");
        return Error{ErrorCode::StorageWriteFailed, ErrorCategory::Storage, "wait patch failed"};
    }
    const int res = cqe->res;
    ::io_uring_cqe_seen(&ring_, cqe);
    if (res < 0 || static_cast<std::size_t>(res) < data.size()) {
        mark_failed(res < 0 ? -res : EIO, "patch_cqe");
        return Error{ErrorCode::StorageWriteFailed, ErrorCategory::Storage, "patch write failed"};
    }
    return {};
}

core::Result<void> IoUringFileSink::finalize() {
    if (fd_ < 0) return {};
    while (!inflight_.empty()) reap(true);
    // fsync so the recording is durable before we report success.
    if (!failed_ && ::fsync(fd_) != 0) {
        mark_failed(errno, "fsync");
    }
    const bool ok = !failed_;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!ok) {
        return Error{ErrorCode::StorageWriteFailed, ErrorCategory::Storage, "finalize saw a prior failure"};
    }
    return {};
}

} // namespace rtmp_server::io::io_uring
