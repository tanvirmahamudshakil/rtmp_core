#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/recording/file_sink.hpp"

namespace rtmp_server::recording {

// Portable (POSIX) FileSink backed by a dedicated writer thread.
//
// Why this exists (docs/v2_promot.md 3.2 justification, see
// docs/recording.md "Why a portable async sink"):
//
//   Role      Before Phase 6 the only concrete FileSink in the tree was
//             io::io_uring::IoUringFileSink, which is compiled exclusively
//             under `NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME ==
//             Linux`. Recorder itself is portable and unit-tested, but it
//             had no production sink to write through anywhere outside a
//             Linux io_uring build.
//   Defect    On any core-only / non-Linux build, recording had no writer
//             at all; and any naive drop-in replacement using plain
//             blocking ::write() on the calling thread would put disk I/O
//             directly on the RTMP command-processing thread, violating
//             docs/v2_promot.md 3.6. A stalled disk would then stall media.
//   Change    Add this sink: the caller (RTMP thread) only ever appends to
//             a bounded in-memory queue and returns; a single owned writer
//             thread performs every ::pwrite/::fsync/::rename. No public
//             method of this class performs disk I/O on the calling thread.
//   Risk      A second sink implementation to keep in step with the
//             FileSink contract. Mitigated by both sinks being exercised
//             through the same Recorder tests, and by this sink being the
//             one the portable build actually uses.
//
// Durability model: bytes are written to `path + ".part"` while recording.
// finalize() flushes, fsync()s, closes and atomically ::rename()s the
// temporary into place, so a reader of `path` never observes a partial
// file and a crash mid-recording leaves an obvious `.part` artifact rather
// than a truncated "finished" recording.
//
// Threading: append/patch/finalize are safe to call from one producer
// thread (the RTMP session that owns the Recorder). Observers
// (pending_bytes/healthy/stats) are safe from any thread.
class AsyncFileSink final : public FileSink {
public:
    struct Options {
        // Hard cap on queued-but-unwritten bytes. append() beyond this
        // fails with ErrorCode::ResourceExhausted (an explicit, counted
        // overflow — never a silent drop). Recorder additionally applies
        // its own softer max_queued_bytes policy via pending_bytes().
        std::size_t max_queue_bytes = 32u * 1024u * 1024u;

        // Refuse to keep writing when the filesystem holding the recording
        // has less than this much free space. Checked by the writer thread
        // (statvfs is a blocking syscall and must not run on the producer),
        // re-evaluated every `disk_check_interval_bytes` written bytes.
        std::uint64_t min_free_bytes = 64u * 1024u * 1024u;
        std::uint64_t disk_check_interval_bytes = 8u * 1024u * 1024u;

        // fsync() before the final rename. Costly but makes a finished
        // recording durable across power loss; on by default.
        bool fsync_on_finalize = true;
    };

    struct Stats {
        std::uint64_t bytes_written = 0;    // bytes durably handed to ::pwrite
        std::uint64_t queue_high_water = 0; // max observed queued bytes
        std::uint64_t overflow_events = 0;  // append() rejections (bounded queue)
        std::uint64_t patches_applied = 0;
        bool finalized = false;             // writer completed close+rename
        bool renamed = false;               // temp -> final rename succeeded
    };

    // Opens `path + ".part"` for writing and starts the writer thread.
    // Returns ErrorCategory::Storage on open failure. Never throws.
    // (Two overloads rather than a defaulted argument: a default member
    // initializer of the nested Options cannot be used in a default argument
    // inside this class's own definition.)
    [[nodiscard]] static core::Result<std::unique_ptr<AsyncFileSink>> open(std::string path,
                                                                          Options options);
    [[nodiscard]] static core::Result<std::unique_ptr<AsyncFileSink>> open(std::string path) {
        return open(std::move(path), Options{});
    }

    ~AsyncFileSink() override;
    AsyncFileSink(const AsyncFileSink&) = delete;
    AsyncFileSink& operator=(const AsyncFileSink&) = delete;
    AsyncFileSink(AsyncFileSink&&) = delete;
    AsyncFileSink& operator=(AsyncFileSink&&) = delete;

    // --- FileSink -------------------------------------------------------
    // All three enqueue and return; none of them touch the disk.
    core::Result<void> append(std::span<const std::byte> data) override;
    core::Result<void> patch(std::uint64_t offset, std::span<const std::byte> data) override;
    core::Result<void> finalize() override;

    [[nodiscard]] std::size_t pending_bytes() const override {
        return queued_bytes_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool healthy() const override { return healthy_.load(std::memory_order_acquire); }

    // --- Observation / lifecycle ----------------------------------------

    // Blocks the CALLING thread until the writer has drained and finalized,
    // or the timeout expires. Intended for tests and for graceful server
    // shutdown — never called from an RTMP media thread. Returns true if
    // finalization completed.
    [[nodiscard]] bool wait_for_completion(std::chrono::milliseconds timeout);

    [[nodiscard]] Stats stats() const;
    // The error that took the sink unhealthy, if any.
    [[nodiscard]] core::Error failure() const;

    // Identity of the thread that performs every write/fsync/rename. Exposed
    // so the "no disk I/O on the RTMP thread" invariant is directly
    // assertable in a test rather than argued in prose: a caller can check
    // that this is never its own thread.
    [[nodiscard]] std::thread::id writer_thread_id() const noexcept { return writer_.get_id(); }

    [[nodiscard]] const std::string& final_path() const noexcept { return final_path_; }
    [[nodiscard]] const std::string& temp_path() const noexcept { return temp_path_; }

    // Phase 7 observability. Non-owning, optional; must be set before the
    // sink starts taking traffic and must outlive the sink. Feeds
    // recording_queue_depth (queued bytes, updated on every enqueue — a
    // single relaxed atomic store, no allocation) and recording_failures
    // (queue overflow and any error that takes the sink unhealthy).
    void set_metrics(observability::Metrics* metrics) noexcept { metrics_ = metrics; }

private:
    AsyncFileSink(int fd, std::string final_path, std::string temp_path, Options options);

    struct Op {
        enum class Kind : std::uint8_t { Append, Patch };
        Kind kind = Kind::Append;
        std::uint64_t offset = 0; // Patch only
        std::vector<std::byte> data;
    };

    void writer_loop();
    // Applies one op with retry on EINTR / partial write. Returns false and
    // marks the sink unhealthy on unrecoverable error.
    bool apply(const Op& op);
    bool check_disk_space();
    void mark_failed(core::Error error);
    void do_finalize();

    int fd_ = -1;
    std::string final_path_;
    std::string temp_path_;
    Options options_;

    mutable std::mutex mutex_;
    std::condition_variable producer_cv_; // writer -> waiters (completion)
    std::condition_variable writer_cv_;   // producer -> writer (work available)
    std::deque<Op> queue_;
    bool finalize_requested_ = false;
    bool finalize_done_ = false;
    Stats stats_;
    core::Error failure_;

    std::atomic<std::size_t> queued_bytes_{0};
    std::atomic<bool> healthy_{true};
    std::uint64_t write_offset_ = 0;      // writer-thread-only
    std::uint64_t bytes_since_disk_check_ = 0; // writer-thread-only

    std::thread writer_;

    observability::Metrics* metrics_ = nullptr; // not owned, may be null
};

} // namespace rtmp_server::recording
