#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "rtmp_server/hls/segment.hpp"
#include "rtmp_server/media/aac/adts.hpp"
#include "rtmp_server/media/h264/avc.hpp"
#include "rtmp_server/media/ts/ts_muxer.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"
#include "rtmp_server/protocol/commands/recorder_sink.hpp"

namespace rtmp_server::hls {

struct SegmenterConfig {
    // Target segment length. A segment is only cut on a video keyframe, so
    // actual durations depend on the publisher's GOP length; a segment is
    // closed at the first keyframe at or after this target.
    std::chrono::milliseconds target_duration{4000};

    // Hard upper bound on a single segment's buffered size. A publisher
    // that never sends another keyframe (or sends a huge one) must not grow
    // this buffer without limit (docs/v2_promot.md 3.5): once exceeded the
    // segment is force-cut at the next access unit, keyframe or not.
    std::size_t max_segment_bytes = 16u * 1024u * 1024u;

    // Force-cut a segment after this much media time even without a
    // keyframe, so a publisher with a pathological GOP cannot stall the
    // playlist indefinitely.
    std::chrono::milliseconds max_segment_duration{20000};

    // A backwards or forwards timestamp jump larger than this is treated as
    // a discontinuity (rollover, encoder reset, or a spliced source) rather
    // than as a real gap in the timeline.
    std::chrono::milliseconds discontinuity_threshold{5000};

    std::string segment_name_prefix = "segment-";
    std::string segment_name_suffix = ".ts";
};

struct SegmenterStats {
    std::uint64_t segments_produced = 0;
    std::uint64_t video_frames = 0;
    std::uint64_t audio_frames = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t dropped_frames = 0;      // frames rejected (no config yet, parse error)
    std::uint64_t forced_cuts = 0;         // cuts made on size/duration bound, not a keyframe
    std::uint64_t sequence_header_changes = 0;
};

// Turns one publishing RTMP stream's FLV-form audio/video payloads into
// MPEG-TS HLS segments. Passthrough only: H.264 and AAC bytes are
// repackaged, never re-encoded (docs/v2_promot.md Phase 6).
//
// Implements protocol::commands::RecorderSink so it can be attached to a
// CommandSession exactly like the FLV Recorder — via TeeRecorderSink when
// both recording and HLS are enabled. All work here is CPU-only buffer
// manipulation (no disk, no syscalls), so it is safe on the media thread;
// the emitted segments are handed to a SegmentStore, and anything that
// touches a disk or a socket happens elsewhere.
class Segmenter final : public protocol::commands::RecorderSink {
public:
    // Invoked once per finished segment, on the calling (media) thread.
    // Must not block — the SegmentStore's implementation only takes a
    // short mutex and does no I/O.
    using SegmentCallback = std::function<void(SegmentPtr)>;

    explicit Segmenter(SegmentCallback on_segment, SegmenterConfig config = {});

    // --- RecorderSink (never throws, never propagates errors) ------------
    void on_metadata(const protocol::chunk::RtmpMessage& message) override;
    void on_audio(const protocol::chunk::RtmpMessage& message) override;
    void on_video(const protocol::chunk::RtmpMessage& message) override;
    void finalize() override;

    // Marks a publisher reconnect: the next segment carries
    // EXT-X-DISCONTINUITY and the TS continuity counters restart.
    void mark_publisher_reconnect();

    [[nodiscard]] const SegmenterStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool has_video_config() const noexcept { return video_config_.has_value(); }
    [[nodiscard]] bool has_audio_config() const noexcept { return audio_config_.has_value(); }
    // Codecs attribute for the master playlist, derived from the SPS
    // profile/level and the AAC object type. Empty until configs arrive.
    [[nodiscard]] std::string codecs_attribute() const;

private:
    // Closes the open segment (if any) and delivers it via the callback.
    void flush_segment();
    // Decides whether `timestamp` continues the current timeline.
    bool is_discontinuous(std::uint32_t timestamp) const;
    void start_segment(std::uint32_t timestamp);
    [[nodiscard]] std::uint64_t to_90k(std::int64_t milliseconds) const;

    // Guards every method below: on_metadata/on_audio/on_video/finalize
    // (RecorderSink) and mark_publisher_reconnect() all read or mutate the
    // shared TS-building state (current_, muxer_, next_sequence_, ...).
    // Originally safe because everything came from one "media thread"; a
    // source-transcode job now decodes/encodes video and audio on their own
    // threads for throughput and pushes into the same Segmenter from both,
    // so this class has to serialize its own state instead of trusting a
    // single-caller assumption that no longer holds. The critical section is
    // just muxing one already-encoded access unit -- short relative to the
    // decode/encode work callers do outside the lock.
    mutable std::mutex mutex_;

    SegmentCallback on_segment_;
    SegmenterConfig config_;
    SegmenterStats stats_;

    media::ts::TsMuxer muxer_;
    std::optional<media::h264::AvcDecoderConfig> video_config_;
    std::optional<media::aac::AudioSpecificConfig> audio_config_;

    std::vector<std::byte> current_;       // TS bytes of the segment being built
    bool segment_open_ = false;
    std::uint32_t segment_start_ts_ = 0;   // RTMP ms at segment start
    std::uint32_t last_ts_ = 0;
    bool have_last_ts_ = false;
    bool pending_discontinuity_ = false;   // apply to the next segment started

    std::uint64_t next_sequence_ = 0;
    // Monotonic timeline base: added to raw RTMP timestamps so PTS/DTS keep
    // increasing across a rollover or reconnect instead of jumping back.
    std::int64_t timeline_base_ms_ = 0;

    bool finalized_ = false;
    // Cached for codecs_attribute().
    std::vector<std::byte> sps_first_bytes_;
};

} // namespace rtmp_server::hls
