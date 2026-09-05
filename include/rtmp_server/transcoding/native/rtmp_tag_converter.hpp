#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/aac/adts.hpp"
#include "rtmp_server/media/h264/avc.hpp"
#include "rtmp_server/media/hevc/hevc.hpp"
#include "rtmp_server/media/media_handoff_queue.hpp" // media::TimestampUnwrapper
#include "rtmp_server/transcoding/native/source_transcoder.hpp"  // SourceVideoCodec

namespace rtmp_server::transcoding::native {

// One decodable video access unit, in the form SourceTranscoder consumes.
// `annexb` points into the converter's own reusable buffer and stays valid
// only until the next convert_* call.
struct ConvertedVideoUnit {
    std::span<const std::byte> annexb;
    std::int64_t pts_90k = 0;
    std::int64_t dts_90k = 0;
    bool keyframe = false;
};

struct ConvertedAudioUnit {
    std::span<const std::byte> adts;
    std::int64_t pts_90k = 0;
};

// Turns RTMP/FLV media tags into the elementary-stream form the native
// transcoder decodes: length-prefixed AVCC (or HVCC) samples become Annex B
// with parameter sets on every keyframe, raw AAC frames get their ADTS header
// back, and 32-bit RTMP millisecond timestamps become a monotonic 90 kHz
// clock.
//
// The three tag shapes a real publisher sends are all handled: classic AVC
// (CodecID 7), the pre-Enhanced-RTMP HEVC convention (CodecID 12, AVC's tag
// layout), and Enhanced RTMP `hvc1` ExVideoTagHeader tags. Sequence headers
// are absorbed into the converter's state rather than returned, so a caller
// only ever sees units it can feed straight to a decoder — that is why the
// return type is an optional unit rather than a unit.
//
// One instance belongs to one media stream and is not thread-safe; the ingest
// path owns one per transcode worker.
class RtmpTagConverter {
public:
    // std::nullopt = the tag carried no picture (sequence header, end of
    // sequence, or a packet type this pipeline ignores). The source codec is
    // learned from the first video tag and reported by video_codec().
    [[nodiscard]] core::Result<std::optional<ConvertedVideoUnit>> convert_video(
        std::span<const std::byte> payload, std::uint32_t timestamp);

    [[nodiscard]] core::Result<std::optional<ConvertedAudioUnit>> convert_audio(
        std::span<const std::byte> payload, std::uint32_t timestamp);

    // The codec the video tags actually carry. Meaningless until the first
    // video tag has been converted (has_video_codec() reports that), and
    // fixed for the life of the stream after it: SourceTranscoder selects its
    // decoder once at construction.
    [[nodiscard]] SourceVideoCodec video_codec() const noexcept { return video_codec_; }
    [[nodiscard]] bool has_video_codec() const noexcept { return video_codec_known_; }

    // True once the parameter sets needed to convert coded frames have been
    // seen. A publisher that reconnects sends them again, and this stays true.
    [[nodiscard]] bool has_video_config() const noexcept;
    [[nodiscard]] bool has_audio_config() const noexcept { return audio_config_.has_value(); }

private:
    [[nodiscard]] core::Result<std::optional<ConvertedVideoUnit>> convert_avc(
        std::span<const std::byte> payload, std::uint32_t timestamp);
    [[nodiscard]] core::Result<std::optional<ConvertedVideoUnit>> convert_hevc(
        std::span<const std::byte> payload, std::uint32_t timestamp, bool enhanced,
        bool keyframe);

    media::h264::AvcDecoderConfig avc_config_;
    std::optional<media::hevc::HevcDecoderConfig> hevc_config_;
    std::optional<media::aac::AudioSpecificConfig> audio_config_;
    SourceVideoCodec video_codec_ = SourceVideoCodec::H264;
    bool video_codec_known_ = false;
    media::TimestampUnwrapper video_clock_;
    media::TimestampUnwrapper audio_clock_;
    // Reused across calls so a live stream does not allocate a fresh buffer
    // per frame; this is why a returned unit is only valid until the next
    // conversion.
    std::vector<std::byte> video_buffer_;
    std::vector<std::byte> audio_buffer_;
};

} // namespace rtmp_server::transcoding::native
