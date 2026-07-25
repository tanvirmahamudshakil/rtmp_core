#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::recording {

// Abstract byte sink the Recorder writes an FLV file through. The Recorder is
// deliberately decoupled from the concrete I/O mechanism behind this
// interface so it is unit-testable without a real disk or a real io_uring
// ring (the production sink, IoUringFileSink, only builds on Linux; tests use
// an in-memory fake). See docs/flv-recording.md "Async writer design".
//
// `append` is the streaming write path (sequential, in order). A concrete
// async sink queues the write and returns immediately; `pending_bytes()`
// reports how many appended-but-not-yet-durably-written bytes are
// outstanding, which the Recorder uses to enforce its bounded queue.
//
// No method throws: I/O errors are reported as core::Result / healthy()==false
// so a disk failure can never escape into an io_uring completion callback
// (docs/rtmp_promot.md Phase 6).
class FileSink {
public:
    virtual ~FileSink() = default;

    // Appends `data` to the end of the file (queued for async write). Returns
    // an error (ErrorCategory::Storage) if the write could not be accepted
    // (e.g. the underlying fd has already failed).
    virtual core::Result<void> append(std::span<const std::byte> data) = 0;

    // Overwrites `data.size()` bytes at absolute `offset` (used to patch the
    // onMetaData duration/filesize placeholders at finalize). Bounded, small.
    virtual core::Result<void> patch(std::uint64_t offset, std::span<const std::byte> data) = 0;

    // Flushes all queued writes and closes the file. Idempotent.
    virtual core::Result<void> finalize() = 0;

    // Bytes appended but not yet confirmed durably written — the depth the
    // Recorder bounds its queue against. A synchronous sink reports 0.
    [[nodiscard]] virtual std::size_t pending_bytes() const { return 0; }

    // False once a write/patch has failed; the Recorder stops feeding a
    // failed sink but never crashes.
    [[nodiscard]] virtual bool healthy() const { return true; }
};

} // namespace rtmp_server::recording
