#pragma once

#include <liburing.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include "rtmp_server/recording/file_sink.hpp"

namespace rtmp_server::io::io_uring {

// io_uring-backed implementation of recording::FileSink: the actual FLV file
// writes go through IORING_OP_WRITE, never a blocking write()/fwrite(), per
// the master spec's io_uring requirement (docs/rtmp_promot.md First
// Milestone). Owns its own small io_uring ring so recording I/O is
// independent of the connection event loop's ring.
//
// Linux-only (needs liburing.h) — it lives in the io_uring target, behind the
// same Linux gate as the rest of the transport. The Recorder itself depends
// only on the abstract FileSink, so recorder logic is exercised on every
// platform via an in-memory fake (docs/flv-recording.md).
//
// Bounded queue: `append` submits asynchronously and reaps any ready
// completions; if outstanding (submitted-but-not-completed) bytes would
// exceed max_inflight_bytes it waits for completions to drain rather than let
// the ring's in-flight buffers grow without limit. The Recorder additionally
// bounds at its own layer via pending_bytes(); the two bounds compose.
class IoUringFileSink final : public recording::FileSink {
public:
    static core::Result<IoUringFileSink> open(const std::string& path,
                                              std::size_t max_inflight_bytes = 16u * 1024u * 1024u,
                                              std::uint32_t queue_depth = 256);

    IoUringFileSink(const IoUringFileSink&) = delete;
    IoUringFileSink& operator=(const IoUringFileSink&) = delete;
    IoUringFileSink(IoUringFileSink&& other) noexcept;
    IoUringFileSink& operator=(IoUringFileSink&& other) noexcept;
    ~IoUringFileSink() override;

    core::Result<void> append(std::span<const std::byte> data) override;
    core::Result<void> patch(std::uint64_t offset, std::span<const std::byte> data) override;
    core::Result<void> finalize() override;
    [[nodiscard]] std::size_t pending_bytes() const override { return inflight_bytes_; }
    [[nodiscard]] bool healthy() const override { return !failed_ && fd_ >= 0; }

private:
    struct Inflight {
        std::uint64_t id;
        std::vector<std::byte> buffer;
    };

    IoUringFileSink(::io_uring ring, int fd, std::size_t max_inflight_bytes)
        : ring_(ring), fd_(fd), max_inflight_bytes_(max_inflight_bytes), valid_(true) {}

    // Reaps ready completions; if `wait` is true, blocks until at least one
    // completion arrives (used to make room / to drain on finalize).
    void reap(bool wait);
    void mark_failed(int error, const char* where);
    void close_ring_and_fd() noexcept;

    ::io_uring ring_{};
    int fd_ = -1;
    std::size_t max_inflight_bytes_ = 0;
    std::uint64_t write_offset_ = 0; // next append offset
    std::size_t inflight_bytes_ = 0;
    std::uint64_t next_id_ = 1;
    std::deque<Inflight> inflight_;
    bool failed_ = false;
    bool valid_ = false;
};

} // namespace rtmp_server::io::io_uring
