#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace rtmp_server::media {

// One RTMP media message on its way from a publisher's media thread to a
// worker that consumes it off-thread (the ingest transcode ladder, a relay or
// stream-target publisher). The payload is copied because the publisher's
// buffer is reused the moment its callback returns; everything else here is
// what the worker needs to reconstruct the tag without re-parsing it twice.
struct HandoffMessage {
    bool video = false;
    // AMF data (onMetaData / @setDataFrame). Carried because a relay or stream
    // target has to reproduce the publisher's own metadata on the far side;
    // treated like audio by the drop policy, since losing it is not recoverable
    // by waiting for the next keyframe.
    bool metadata = false;
    // Only meaningful for video. Set by the producer from the FLV frame-type
    // nibble, before the message is queued, so the drop policy below can
    // recognise a resynchronisation point without decoding anything.
    bool keyframe = false;
    // Also only meaningful for video: an AVC/HEVC sequence header carries no
    // pictures but must never be dropped, or every later frame in the stream
    // is undecodable.
    bool sequence_header = false;
    std::uint32_t timestamp = 0;
    std::vector<std::byte> payload;
};

struct HandoffLimits {
    // Both are ceilings on what may sit between the two threads, not targets.
    // A live ladder that is keeping up holds well under one GOP here; these
    // only bound how much memory a *publisher* can pin when the encoders fall
    // behind real time (a CPU-starved box, a rendition ladder too large for
    // the machine), which is the case that otherwise grows without limit and
    // takes the whole process down with it.
    std::size_t max_bytes = 32u * 1024u * 1024u;
    std::size_t max_messages = 2048;
};

struct HandoffStats {
    std::uint64_t pushed = 0;
    std::uint64_t dropped = 0;
    // How many times the queue gave up on the current GOP and restarted from
    // a keyframe. One resync is one visible skip for the transcoded ladder,
    // so this is the number an operator should watch, not `dropped`.
    std::uint64_t resyncs = 0;
    std::size_t queued_bytes = 0;
    std::size_t queued_messages = 0;
};

// Bounded hand-off between a publishing connection's media thread and a
// worker thread that does something slow with the same media: re-encoding it
// into a rendition ladder, or pushing it to another server.
//
// The publisher must never block: it is driven by an io_uring worker that also
// serves every other connection on that ring, so a slow encoder must cost
// transcoded quality, never ingest or passthrough delivery. When the worker
// cannot keep up, this queue therefore drops rather than waits, and it drops
// on decode boundaries: once a video frame is discarded, every following frame
// is discarded until the next keyframe, because feeding a decoder the tail of
// a GOP whose start is missing produces corrupt pictures rather than a clean
// gap. Audio and sequence headers are never dropped for that reason -- a lost
// AudioSpecificConfig or AVCDecoderConfigurationRecord breaks the rest of the
// session, not just one GOP.
class MediaHandoffQueue {
public:
    explicit MediaHandoffQueue(HandoffLimits limits = {}) : limits_(limits) {}

    // Called on the publisher's media thread. Returns false when the message
    // was dropped. Never blocks.
    bool push(HandoffMessage message);

    // Called on the consuming worker. Blocks until a message is available or
    // the queue is closed; returns false only once closed and drained.
    bool pop(HandoffMessage& out);

    // Same, but gives up after `timeout` so a worker that also has to service
    // a socket (the relay publisher polling for acknowledgements) can wait on
    // media without going deaf to its connection.
    bool pop_for(HandoffMessage& out, std::chrono::milliseconds timeout);

    // Wakes and permanently stops `pop`. Safe to call more than once.
    void close();

    // True exactly once per resync: the worker uses it to re-anchor the
    // transcoder's output clock over the gap it is about to see.
    [[nodiscard]] bool take_resync();

    [[nodiscard]] HandoffStats stats() const;

private:
    // Caller holds mutex_.
    [[nodiscard]] bool over_limit_locked(std::size_t incoming_bytes) const;
    void clear_locked();

    HandoffLimits limits_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<HandoffMessage> queue_;
    std::size_t queued_bytes_ = 0;
    bool closed_ = false;
    // Set when a video frame has been dropped and no keyframe has arrived
    // since; while set, every non-keyframe video message is dropped too.
    bool awaiting_keyframe_ = false;
    bool resync_pending_ = false;
    std::uint64_t pushed_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t resyncs_ = 0;
};

// RTMP timestamps are 32-bit milliseconds and wrap; a consumer that needs a
// monotonic input clock (the transcoder, a relay re-basing its output
// timeline) unwraps them through this.
class TimestampUnwrapper {
public:
    std::uint64_t unwrap(std::uint32_t value) {
        if (last_ && value < *last_ && static_cast<std::uint32_t>(*last_ - value) > 0x80000000u) {
            epoch_ += (std::uint64_t{1} << 32);
        }
        last_ = value;
        return epoch_ + value;
    }

private:
    std::optional<std::uint32_t> last_;
    std::uint64_t epoch_ = 0;
};

} // namespace rtmp_server::media
