#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/aac_decoder.hpp"
#include "rtmp_server/transcoding/native/aac_encoder.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/h264_decoder.hpp"
#include "rtmp_server/transcoding/native/h264_encoder.hpp"
#include "rtmp_server/transcoding/native/scaler.hpp"

namespace rtmp_server::transcoding::native {

// One rendition of the output ladder, derived from a transcoding-template preset.
struct RenditionSpec {
    std::string name;
    std::uint32_t width = 0;  // 0 = keep source width
    std::uint32_t height = 0; // 0 = keep source height
    std::uint32_t video_bitrate = 2'500'000;
    std::uint32_t gop = 60;
    std::uint32_t audio_bitrate = 128'000;
};

// The FFmpeg-free source transcoder core. It decodes a source's H.264/AAC
// elementary units once, then fans the decoded picture/PCM out to every
// rendition — each with its own scaler + H.264 encoder (and AAC encoder) — so
// adding a resolution never adds a decode. This is the "decode once, encode per
// rendition in parallel" shape; threading/queueing lives in the caller, this
// object is the deterministic per-frame core (easy to unit-test).
//
// Feed it the output of TsDemuxer (HLS/TS source) or the RTMP media path. It is
// resolution-agnostic: encoders are built lazily once the true source size is
// known, so template presets that keep the source aspect ratio resolve right.
class SourceTranscoder {
public:
    // Encoded video for one rendition (Annex B) — ready for the segmenter.
    using VideoOutput =
        std::function<void(std::size_t rendition, const EncodedAccessUnit& access_unit)>;
    // Encoded audio for one rendition (ADTS AAC).
    using AudioOutput =
        std::function<void(std::size_t rendition, const EncodedAudioFrame& frame,
                           std::int64_t pts_90k)>;

    SourceTranscoder(std::vector<RenditionSpec> renditions, std::uint32_t fps);
    ~SourceTranscoder();
    SourceTranscoder(const SourceTranscoder&) = delete;
    SourceTranscoder& operator=(const SourceTranscoder&) = delete;

    void set_video_output(VideoOutput handler) { video_output_ = std::move(handler); }
    void set_audio_output(AudioOutput handler) { audio_output_ = std::move(handler); }

    [[nodiscard]] core::Result<void> start();

    // One source H.264 access unit (Annex B). Decodes, then scales + encodes for
    // every rendition, invoking the video output per rendition.
    [[nodiscard]] core::Result<void> on_video(std::span<const std::byte> annexb,
                                              std::int64_t pts_90k, std::int64_t dts_90k,
                                              bool keyframe);
    // One source ADTS AAC frame. Decodes, then re-encodes per rendition.
    [[nodiscard]] core::Result<void> on_audio(std::span<const std::byte> adts,
                                              std::int64_t pts_90k);

    [[nodiscard]] std::size_t rendition_count() const noexcept { return renditions_.size(); }

private:
    struct Rendition {
        RenditionSpec spec;
        Scaler scaler;
        H264Encoder video_encoder;
        AacEncoder audio_encoder;
        YuvFrame scaled;
        bool video_open = false;
        bool audio_open = false;
        std::int64_t audio_base_pts_90k = 0;
        bool audio_base_set = false;
        std::uint64_t audio_samples = 0;
    };

    [[nodiscard]] core::Result<void> ensure_video(Rendition& rendition, std::uint32_t src_w,
                                                  std::uint32_t src_h);

    std::vector<RenditionSpec> specs_;
    std::uint32_t fps_ = 30;
    H264Decoder video_decoder_;
    AacDecoder audio_decoder_;
    YuvFrame decoded_;
    PcmBlock pcm_;
    std::vector<std::unique_ptr<Rendition>> renditions_;
    VideoOutput video_output_;
    AudioOutput audio_output_;
    bool started_ = false;
    bool audio_configured_ = false;
};

} // namespace rtmp_server::transcoding::native
