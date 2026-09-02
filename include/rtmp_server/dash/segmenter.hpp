#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rtmp_server/dash/segment.hpp"
#include "rtmp_server/media/aac/adts.hpp"
#include "rtmp_server/media/h264/avc.hpp"
#include "rtmp_server/media/h264/sps.hpp"
#include "rtmp_server/media/mp4/fmp4_muxer.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"
#include "rtmp_server/protocol/commands/recorder_sink.hpp"

namespace rtmp_server::dash {

struct SegmenterConfig {
    // Target segment length, same role as hls::SegmenterConfig::target_duration:
    // a segment is only cut on a video keyframe, so real durations follow the
    // publisher's GOP length.
    std::chrono::milliseconds target_duration{4000};
    std::size_t max_segment_bytes = 16u * 1024u * 1024u;
    std::chrono::milliseconds max_segment_duration{20000};
    std::chrono::milliseconds discontinuity_threshold{5000};

    std::string segment_name_prefix = "chunk-";
    std::string segment_name_suffix = ".m4s";
    std::uint64_t initial_sequence = 0;
};

struct SegmenterStats {
    std::uint64_t segments_produced = 0;
    std::uint64_t video_frames = 0;
    std::uint64_t audio_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t forced_cuts = 0;
    std::uint64_t init_segments_produced = 0; // codec-configuration epochs
};

// Turns one publishing RTMP stream's FLV-form audio/video payloads into
// fragmented MP4 (CMAF) segments for DASH delivery — the fMP4 counterpart of
// hls::Segmenter. Passthrough only, H.264 + AAC (the same restriction
// hls::Segmenter has): video AVCC samples and audio raw AAC frames are
// repackaged, never re-encoded or re-framed. Unlike the HLS/TS path, fMP4
// keeps FLV's own AVCC sample layout and raw AAC frames verbatim — no Annex B
// conversion, no ADTS framing — because media::mp4::Fmp4Muxer wants exactly
// that shape.
//
// Implements protocol::commands::RecorderSink so it composes with the HLS
// Segmenter and the FLV Recorder via TeeRecorderSink, all three fed from one
// publisher's media without re-parsing it three times.
class Segmenter final : public protocol::commands::RecorderSink {
public:
    using SegmentCallback = std::function<void(SegmentPtr)>;
    // Invoked whenever the init segment is (re)built — once for the first
    // valid codec configuration, and again on a mid-stream parameter change.
    using InitCallback = std::function<void(InitSegmentPtr)>;

    Segmenter(SegmentCallback on_segment, InitCallback on_init, SegmenterConfig config = {});

    void on_metadata(const protocol::chunk::RtmpMessage& message) override;
    void on_audio(const protocol::chunk::RtmpMessage& message) override;
    void on_video(const protocol::chunk::RtmpMessage& message) override;
    void finalize() override;

    void mark_publisher_reconnect();

    [[nodiscard]] const SegmenterStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool has_video_config() const noexcept { return video_config_.has_value(); }
    [[nodiscard]] bool has_audio_config() const noexcept { return audio_config_.has_value(); }
    // Codecs attribute for the MPD Representation, derived the same way as
    // hls::Segmenter::codecs_attribute(). Empty until configs arrive.
    [[nodiscard]] std::string codecs_attribute() const;

private:
    struct PendingSample {
        std::vector<std::byte> data;
        std::uint32_t timestamp_ms = 0;
        std::int32_t composition_time_ms = 0;
        bool keyframe = false;
        bool has = false;
    };

    struct Track {
        std::vector<std::vector<std::byte>> owned_data; // one entry per sample
        std::vector<media::mp4::Fmp4Sample> samples;     // spans into owned_data
        PendingSample pending;
        std::uint32_t last_duration_90k = 0; // carried forward for the tail sample
        std::uint64_t base_decode_time_90k = 0;
        bool base_set = false;

        void clear() {
            owned_data.clear();
            samples.clear();
        }
        [[nodiscard]] bool empty() const noexcept { return samples.empty(); }
    };

    void flush_segment();
    void rebuild_init_segment();
    // Moves `pending` into `track`'s sample list with the given duration, and
    // fixes up the just-pushed sample's span (owned_data may have
    // reallocated on this call, so every prior span is rebuilt too).
    void commit_pending(Track& track, std::uint32_t duration_90k);
    void close_track_tail(Track& track);
    [[nodiscard]] std::uint64_t to_90k(std::int64_t milliseconds) const;
    void rebuild_sample_spans(Track& track);

    SegmentCallback on_segment_;
    InitCallback on_init_;
    SegmenterConfig config_;
    SegmenterStats stats_;

    media::mp4::Fmp4Muxer muxer_;
    std::optional<media::h264::AvcDecoderConfig> video_config_;
    std::optional<media::aac::AudioSpecificConfig> audio_config_;
    std::vector<std::byte> video_decoder_config_raw_; // avcC box body, cached verbatim
    std::vector<std::byte> audio_specific_config_raw_;
    media::VideoDimensions video_dimensions_;
    std::uint64_t init_epoch_ = 0;

    Track video_track_;
    Track audio_track_;
    bool segment_open_ = false;
    std::uint32_t segment_start_ts_ = 0;
    std::uint32_t last_video_ts_ = 0;
    bool have_last_video_ts_ = false;
    std::size_t segment_byte_estimate_ = 0;

    std::uint64_t next_sequence_ = 0;
    std::int64_t timeline_base_ms_ = 0;
    bool finalized_ = false;
};

} // namespace rtmp_server::dash
