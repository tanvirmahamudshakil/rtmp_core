#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/core/thread_pool.hpp"
#include "rtmp_server/transcoding/native/aac_decoder.hpp"
#include "rtmp_server/transcoding/native/aac_encoder.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/h264_decoder.hpp"
#include "rtmp_server/transcoding/native/h264_encoder.hpp"
#include "rtmp_server/transcoding/native/hevc_decoder.hpp"
#include "rtmp_server/transcoding/native/scaler.hpp"
#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding::native {

// Which codec the source's video elementary stream is encoded in. Decode is
// selected on this; it is independent of each rendition's own output codec
// (a HEVC source can still fan out to H.264-encoded renditions, and vice
// versa -- see RenditionSpec/Rendition::video_encoder, which is unaffected by
// this choice).
enum class SourceVideoCodec {
    H264,
    Hevc,
};

// One rendition of the output ladder, derived from a transcoding-template preset.
struct RenditionSpec {
    std::string name;           // preset/label
    std::string output_stream;  // output stream key (e.g. "restream_720p")
    std::uint32_t width = 0;  // 0 = keep source width
    std::uint32_t height = 0; // 0 = keep source height
    std::uint32_t video_bitrate = 2'500'000;
    std::uint32_t gop = 60;
    std::uint32_t audio_bitrate = 128'000;
    FitMode fit_mode = FitMode::Stretch;
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

    SourceTranscoder(std::vector<RenditionSpec> renditions, std::uint32_t fps,
                     SourceVideoCodec video_codec = SourceVideoCodec::H264);
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

    // Call after a source reconnect/gap. The next decoded frame re-anchors
    // input sampling, while the generated output clock remains monotonic.
    // Resetting the output clock to the source's new raw PTS here produces
    // non-monotonic DTS and long apparent freezes in VLC.
    // Also re-anchors every rendition's audio clock (audio_base_set = false),
    // so the next audio frame re-anchors to video's output position at that
    // point -- see on_audio's comment on why it anchors to video's clock
    // rather than to its own raw incoming PTS.
    void mark_discontinuity() noexcept {
        awaiting_video_reanchor_ = video_clock_set_;
        next_input_video_pts_90k_ = 0;
        consecutive_backward_drops_ = 0;
        for (auto& rendition : renditions_) {
            rendition->audio_base_set = false;
            rendition->audio_samples = 0;
        }
    }

private:
    struct Rendition {
        RenditionSpec spec;
        Scaler scaler;
        H264Encoder video_encoder;
        AacEncoder audio_encoder;
        YuvFrame scaled;
        bool video_open = false;
        bool audio_open = false;
        std::uint32_t video_threads = 1;
        std::int64_t audio_base_pts_90k = 0;
        bool audio_base_set = false;
        std::uint64_t audio_samples = 0;
    };

    [[nodiscard]] core::Result<void> ensure_video(Rendition& rendition, std::uint32_t src_w,
                                                  std::uint32_t src_h);

    std::vector<RenditionSpec> specs_;
    std::uint32_t fps_ = 30;
    SourceVideoCodec video_codec_ = SourceVideoCodec::H264;
    // The H.264 branch (video_decoder_.emplace<H264Decoder>()) is unchanged
    // from before this variant existed: same type, same calls, same order.
    // HevcDecoder is a parallel alternative selected once at construction,
    // never switched mid-stream.
    std::variant<H264Decoder, HevcDecoder> video_decoder_;
    AacDecoder audio_decoder_;
    YuvFrame decoded_;
    PcmBlock pcm_;
    std::vector<std::unique_ptr<Rendition>> renditions_;
    VideoOutput video_output_;
    AudioOutput audio_output_;
    bool started_ = false;
    bool audio_configured_ = false;
    bool video_clock_set_ = false;
    bool awaiting_video_reanchor_ = false;
    std::int64_t last_input_video_pts_90k_ = 0;
    std::int64_t next_output_video_pts_90k_ = 0;
    // Absolute input-PTS deadline for output frame sampling. Unlike a delta
    // accumulator this tolerates 30fps's normal 2970/3060 tick rounding
    // without dropping a third of the pictures.
    std::int64_t next_input_video_pts_90k_ = 0;
    // Consecutive frames dropped by on_video's backward-discontinuity gate;
    // once this covers about half a second of real frames, the gate treats
    // it as an unflagged discontinuity instead of waiting out the full
    // tolerance (see on_video's comment on the gate itself).
    std::int64_t consecutive_backward_drops_ = 0;

    // Scale+encode is CPU-bound per rendition; fanning renditions across a
    // pool (rather than looping them serially on this call's thread) is what
    // lets a 3-4 rendition ladder use several cores per frame instead of
    // pegging one core and falling behind real time. Sized to renditions_
    // in start(), capped by hardware_concurrency().
    std::unique_ptr<core::ThreadPool> render_pool_;
};

} // namespace rtmp_server::transcoding::native
