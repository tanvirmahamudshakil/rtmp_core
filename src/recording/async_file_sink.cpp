#include "rtmp_server/recording/async_file_sink.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::recording {

namespace {

using observability::LogField;
using observability::LogLevel;

core::Error storage_error(core::ErrorCode code, std::string_view where, int err) {
    std::string message(where);
    message += ": ";
    message += std::strerror(err);
    return core::Error(code, core::ErrorCategory::Storage, message);
}

} // namespace

core::Result<std::unique_ptr<AsyncFileSink>> AsyncFileSink::open(std::string path, Options options) {
    if (path.empty()) {
        return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Storage,
                           "recording path is empty");
    }
    if (options.max_queue_bytes == 0) {
        return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Storage,
                           "max_queue_bytes must be non-zero");
    }

    std::string temp = path + ".part";
    // O_TRUNC: a leftover .part from a crashed run is replaced, not appended to.
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return storage_error(core::ErrorCode::StorageUnavailable, "open(" + temp + ")", errno);
    }

    // Constructed with `new` because the constructor is private; the thread
    // is started after construction so `this` is fully initialised first.
    std::unique_ptr<AsyncFileSink> sink(
        new AsyncFileSink(fd, std::move(path), std::move(temp), options));
    AsyncFileSink* raw = sink.get();
    sink->writer_ = std::thread([raw] { raw->writer_loop(); });
    return std::move(sink);
}

AsyncFileSink::AsyncFileSink(int fd, std::string final_path, std::string temp_path, Options options)
    : fd_(fd),
      final_path_(std::move(final_path)),
      temp_path_(std::move(temp_path)),
      options_(options) {}

AsyncFileSink::~AsyncFileSink() {
    // RAII: guarantee the writer thread is stopped and the fd released even
    // if the owner never called finalize(). Never detach (3.3).
    {
        std::lock_guard lock(mutex_);
        finalize_requested_ = true;
    }
    writer_cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

core::Result<void> AsyncFileSink::append(std::span<const std::byte> data) {
    if (data.empty()) return {};
    if (!healthy_.load(std::memory_order_acquire)) return failure();

    {
        std::unique_lock lock(mutex_);
        if (finalize_requested_) {
            return core::Error(core::ErrorCode::InvalidStateTransition, core::ErrorCategory::Storage,
                               "append after finalize");
        }
        const std::size_t queued = queued_bytes_.load(std::memory_order_relaxed);
        if (queued + data.size() > options_.max_queue_bytes) {
            // Explicit, counted overflow. ResourceExhausted is distinct from
            // a write failure so the Recorder can treat it as backpressure
            // (drop this frame, keep recording) rather than abort.
            stats_.overflow_events += 1;
            if (metrics_ != nullptr) metrics_->increment(observability::MetricId::RecordingFailures);
            return core::Error(core::ErrorCode::ResourceExhausted, core::ErrorCategory::Storage,
                               "recording queue full");
        }

        Op op;
        op.kind = Op::Kind::Append;
        op.data.assign(data.begin(), data.end());
        queue_.push_back(std::move(op));
        const std::size_t now = queued + data.size();
        queued_bytes_.store(now, std::memory_order_release);
        if (now > stats_.queue_high_water) stats_.queue_high_water = now;
        // recording_queue_depth is a live gauge of queued-but-unwritten
        // bytes: one relaxed atomic store, no allocation, so it is safe to
        // publish from the producer (RTMP) thread.
        if (metrics_ != nullptr) {
            metrics_->set(observability::MetricId::RecordingQueueDepth, static_cast<std::int64_t>(now));
        }
    }
    writer_cv_.notify_one();
    return {};
}

core::Result<void> AsyncFileSink::patch(std::uint64_t offset, std::span<const std::byte> data) {
    if (data.empty()) return {};
    if (!healthy_.load(std::memory_order_acquire)) return failure();

    {
        std::unique_lock lock(mutex_);
        // Patches are queued in the same FIFO as appends so a patch can
        // never overtake the appends it is meant to overwrite.
        const std::size_t queued = queued_bytes_.load(std::memory_order_relaxed);
        if (queued + data.size() > options_.max_queue_bytes) {
            stats_.overflow_events += 1;
            if (metrics_ != nullptr) metrics_->increment(observability::MetricId::RecordingFailures);
            return core::Error(core::ErrorCode::ResourceExhausted, core::ErrorCategory::Storage,
                               "recording queue full (patch)");
        }
        Op op;
        op.kind = Op::Kind::Patch;
        op.offset = offset;
        op.data.assign(data.begin(), data.end());
        queue_.push_back(std::move(op));
        queued_bytes_.store(queued + data.size(), std::memory_order_release);
        if (metrics_ != nullptr) {
            metrics_->set(observability::MetricId::RecordingQueueDepth,
                          static_cast<std::int64_t>(queued + data.size()));
        }
    }
    writer_cv_.notify_one();
    return {};
}

core::Result<void> AsyncFileSink::finalize() {
    // Non-blocking by design: this is called from the RTMP command thread on
    // unpublish/disconnect. It only sets a flag; the writer thread drains
    // the queue, fsyncs, closes and renames. Callers that must observe the
    // result (shutdown, tests) use wait_for_completion().
    {
        std::lock_guard lock(mutex_);
        if (finalize_requested_) return {};
        finalize_requested_ = true;
    }
    writer_cv_.notify_all();
    return {};
}

bool AsyncFileSink::wait_for_completion(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return producer_cv_.wait_for(lock, timeout, [this] { return finalize_done_; });
}

AsyncFileSink::Stats AsyncFileSink::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

core::Error AsyncFileSink::failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
}

void AsyncFileSink::mark_failed(core::Error error) {
    // Caller must NOT hold mutex_.
    {
        std::lock_guard lock(mutex_);
        if (!failure_.ok()) return;
        failure_ = error;
    }
    healthy_.store(false, std::memory_order_release);
    if (metrics_ != nullptr) metrics_->increment(observability::MetricId::RecordingFailures);
    RTMP_LOG(LogLevel::Error, "recording", "sink_failed",
             {LogField{"path", temp_path_}, LogField{"error", error.message()}});
}

bool AsyncFileSink::check_disk_space() {
    struct ::statvfs vfs {};
    if (::statvfs(temp_path_.c_str(), &vfs) != 0) {
        // Not fatal: an unavailable statvfs must not kill a working
        // recording. Report once and keep writing.
        RTMP_LOG(LogLevel::Warn, "recording", "statvfs_failed",
                 {LogField{"path", temp_path_}, LogField{"errno", std::string(std::strerror(errno))}});
        return true;
    }
    const auto free_bytes = static_cast<std::uint64_t>(vfs.f_bavail) *
                            static_cast<std::uint64_t>(vfs.f_frsize);
    if (free_bytes < options_.min_free_bytes) {
        mark_failed(core::Error(core::ErrorCode::StorageUnavailable, core::ErrorCategory::Storage,
                                "free disk space " + std::to_string(free_bytes) +
                                    " below configured minimum " +
                                    std::to_string(options_.min_free_bytes)));
        return false;
    }
    return true;
}

bool AsyncFileSink::apply(const Op& op) {
    const std::uint64_t offset = (op.kind == Op::Kind::Patch) ? op.offset : write_offset_;
    const std::byte* data = op.data.data();
    std::size_t remaining = op.data.size();
    std::uint64_t at = offset;

    while (remaining > 0) {
        // pwrite (not write) so a patch can never disturb the append cursor.
        const ssize_t n = ::pwrite(fd_, data, remaining, static_cast<off_t>(at));
        if (n < 0) {
            if (errno == EINTR) continue; // retry, not an error (3.4)
            mark_failed(storage_error(core::ErrorCode::StorageWriteFailed, "pwrite", errno));
            return false;
        }
        if (n == 0) {
            mark_failed(core::Error(core::ErrorCode::StorageWriteFailed, core::ErrorCategory::Storage,
                                    "pwrite made no progress"));
            return false;
        }
        // A short write is normal (e.g. a nearly-full filesystem): advance
        // and continue rather than assuming the whole buffer landed.
        const auto written = static_cast<std::size_t>(n);
        data += written;
        remaining -= written;
        at += written;
    }

    if (op.kind == Op::Kind::Append) {
        write_offset_ += op.data.size();
        std::lock_guard lock(mutex_);
        stats_.bytes_written += op.data.size();
    } else {
        std::lock_guard lock(mutex_);
        stats_.patches_applied += 1;
    }
    return true;
}

void AsyncFileSink::do_finalize() {
    bool renamed = false;
    if (healthy_.load(std::memory_order_acquire)) {
        if (options_.fsync_on_finalize && fd_ >= 0) {
            while (::fsync(fd_) != 0) {
                if (errno == EINTR) continue;
                mark_failed(storage_error(core::ErrorCode::StorageWriteFailed, "fsync", errno));
                break;
            }
        }
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    // Atomic publish: only a fully-written recording ever appears at the
    // final path. On the failure path the .part file is deliberately left
    // behind as evidence rather than published as a good recording.
    if (healthy_.load(std::memory_order_acquire)) {
        if (::rename(temp_path_.c_str(), final_path_.c_str()) == 0) {
            renamed = true;
        } else {
            mark_failed(storage_error(core::ErrorCode::StorageWriteFailed, "rename", errno));
        }
    }

    {
        std::lock_guard lock(mutex_);
        stats_.renamed = renamed;
        stats_.finalized = true;
        finalize_done_ = true;
    }
    producer_cv_.notify_all();

    RTMP_LOG(LogLevel::Info, "recording", "sink_finalized",
             {LogField{"path", final_path_},
              LogField{"renamed", renamed ? "true" : "false"},
              LogField{"bytes", std::to_string(write_offset_)}});
}

void AsyncFileSink::writer_loop() {
    for (;;) {
        Op op;
        {
            std::unique_lock lock(mutex_);
            writer_cv_.wait(lock, [this] { return !queue_.empty() || finalize_requested_; });

            if (queue_.empty()) {
                // finalize_requested_ and fully drained.
                lock.unlock();
                do_finalize();
                return;
            }

            op = std::move(queue_.front());
            queue_.pop_front();
            const std::size_t queued = queued_bytes_.load(std::memory_order_relaxed);
            const std::size_t remaining = queued - op.data.size();
            queued_bytes_.store(remaining, std::memory_order_release);
            if (metrics_ != nullptr) {
                metrics_->set(observability::MetricId::RecordingQueueDepth, static_cast<std::int64_t>(remaining));
            }
        }

        if (!healthy_.load(std::memory_order_acquire)) {
            // Already failed: drain the queue without touching the disk so
            // producers are not blocked and memory is released promptly.
            continue;
        }

        if (!apply(op)) continue;

        bytes_since_disk_check_ += op.data.size();
        if (bytes_since_disk_check_ >= options_.disk_check_interval_bytes) {
            bytes_since_disk_check_ = 0;
            (void)check_disk_space();
        }
    }
}

} // namespace rtmp_server::recording
