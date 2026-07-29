#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rtmp_server::protocol::commands {

// Hard caps on one viewer's outstanding backlog, config-driven
// (core::ServerConfig::subscriber_queue_max_*).
struct QueueLimits {
    std::uint64_t max_bytes = 8 * 1024 * 1024;
    std::uint32_t max_packets = 1000;
};

// Bytes and complete RTMP messages still queued for one viewer. Tracking
// both dimensions prevents many tiny audio/control messages from bypassing
// a byte-only limit.
struct QueueBacklog {
    std::size_t bytes = 0;
    std::size_t packets = 0;
};

// Bounded per-viewer backpressure tracker implementing the staged
// slow-viewer policy from docs/v2_promot.md PHASE 3:
//
//   Normal -> (queue limit exceeded) drop non-key video frames ->
//   WaitingForKeyframe -> discard video (and audio, see below) until next
//   keyframe -> send latest sequence headers + resume from keyframe ->
//   (still stuck) Disconnect.
//
// Audio policy (explicitly documented per the doc's requirement): audio is
// never buffered indefinitely and is not exempt from backpressure — while
// WaitingForKeyframe, audio frames are dropped by the same rule as
// non-keyframe video (offer() takes an `is_video` flag; audio always
// passes is_video=false, is_keyframe=false, so it takes the same
// DropAndWait branch as a non-key video frame). There is no separate,
// larger allowance for audio.
//
// Transport-independent: `external_pending_bytes` lets a caller fold in a
// real socket write-backlog signal (e.g. TcpConnection's queued-but-unsent
// bytes) on top of this object's own bytes()/packet_count() bookkeeping;
// passing 0 makes this a fully self-contained, unit-testable object (used
// by LiveFanout, which has no transport of its own).
class ViewerQueue {
public:
    enum class State : std::uint8_t { Normal, WaitingForKeyframe };

    // Deliver: send the frame as-is.
    // DeliverResumed: send the frame, but the caller must first (re-)send
    //   the latest sequence headers — this subscriber was WaitingForKeyframe
    //   and this frame is the keyframe that lets it resume.
    // DropAndWait: do not deliver; still waiting for a keyframe (or the
    //   viewer is over its budget and has just entered that state).
    // Evict: this subscriber failed to recover after
    //   max_frames_waiting_for_keyframe consecutive drops; disconnect it.
    enum class Decision : std::uint8_t { Deliver, DeliverResumed, DropAndWait, Evict };

    explicit ViewerQueue(QueueLimits limits = {}, std::size_t max_frames_waiting_for_keyframe = 250)
        : limits_(limits), max_frames_waiting_for_keyframe_(max_frames_waiting_for_keyframe) {}

    [[nodiscard]] Decision offer(QueueBacklog external, std::size_t frame_bytes, bool is_video,
                                  bool is_keyframe) {
        if (state_ == State::WaitingForKeyframe) {
            // Do not add a large keyframe to a socket that is still backed
            // up. Resume only after the real transport queue drains below a
            // low watermark; this hysteresis prevents an unbounded
            // keyframe-per-GOP sawtooth on permanently slow clients.
            if (is_video && is_keyframe && below_resume_watermark(external)) {
                state_ = State::Normal;
                frames_waited_ = 0;
                // The resuming keyframe itself is a free pass (not counted
                // against the budget): it is exactly one frame, whatever its
                // size, and forcing it through the same accounting a
                // regular frame would use could immediately re-trip the
                // staged policy before the viewer gets a real chance to
                // catch up. Subsequent frames are accounted normally.
                reset_counters();
                return Decision::DeliverResumed;
            }
            if (++frames_waited_ > max_frames_waiting_for_keyframe_) {
                return Decision::Evict;
            }
            return Decision::DropAndWait; // covers both non-key video and all audio, see class doc
        }

        // Already over budget *before* this frame (persisted from a prior
        // call — normally shouldn't happen since crossing the threshold
        // below immediately resets counters, but guards against a caller
        // supplying a growing external_pending_bytes between calls): drop
        // this frame outright rather than let an already-backed-up viewer
        // accept one more.
        if (over_budget(external, 0)) {
            state_ = State::WaitingForKeyframe;
            frames_waited_ = 0;
            reset_counters();
            return Decision::DropAndWait;
        }

        // Deliver this frame — even if it is the one that pushes the viewer
        // over budget, so a viewer never has a frame silently withheld
        // purely for arriving at the wrong accounting moment — then, if it
        // did cross the threshold, shed the backlog and start waiting for
        // the next keyframe from here on.
        record(frame_bytes);
        if (over_budget(external, 0)) {
            state_ = State::WaitingForKeyframe;
            frames_waited_ = 0;
            reset_counters();
        }
        return Decision::Deliver;
    }

    [[nodiscard]] Decision offer(std::size_t external_pending_bytes, std::size_t frame_bytes, bool is_video,
                                  bool is_keyframe) {
        return offer(QueueBacklog{external_pending_bytes, 0}, frame_bytes, is_video, is_keyframe);
    }

    // Caller-driven accounting hook: called once a previously-delivered
    // frame's bytes are confirmed drained by the transport (or, for a
    // transport-less caller like LiveFanout whose delivery is synchronous,
    // immediately after offer() returns Deliver/DeliverResumed) so bytes()/
    // packet_count() reflect steady-state backlog rather than growing
    // unbounded for the lifetime of the subscription.
    void note_flushed(std::size_t frame_bytes) {
        bytes_ = frame_bytes >= bytes_ ? 0 : bytes_ - frame_bytes;
        if (packet_count_ > 0) --packet_count_;
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint32_t packet_count() const noexcept { return packet_count_; }

private:
    [[nodiscard]] bool over_budget(QueueBacklog external, std::size_t extra_frame_bytes) const {
        std::uint64_t total_bytes = bytes_ + external.bytes + extra_frame_bytes;
        std::uint64_t total_packets = packet_count_ + external.packets;
        bool over_bytes = limits_.max_bytes != 0 && total_bytes > limits_.max_bytes;
        bool over_packets = limits_.max_packets != 0 && total_packets > limits_.max_packets;
        return over_bytes || over_packets;
    }

    [[nodiscard]] bool below_resume_watermark(QueueBacklog external) const {
        const bool bytes_ready =
            limits_.max_bytes == 0 || external.bytes <= std::max<std::uint64_t>(1, limits_.max_bytes / 2);
        const bool packets_ready =
            limits_.max_packets == 0 || external.packets <= std::max<std::uint32_t>(1, limits_.max_packets / 2);
        return bytes_ready && packets_ready;
    }

    void record(std::size_t frame_bytes) {
        bytes_ += frame_bytes;
        ++packet_count_;
    }
    void reset_counters() {
        bytes_ = 0;
        packet_count_ = 0;
    }

    QueueLimits limits_;
    std::size_t max_frames_waiting_for_keyframe_;
    State state_ = State::Normal;
    std::size_t frames_waited_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint32_t packet_count_ = 0;
};

} // namespace rtmp_server::protocol::commands
