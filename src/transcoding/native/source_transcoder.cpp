#include "rtmp_server/transcoding/native/source_transcoder.hpp"

#include <algorithm>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/transcoding/native/aac_params.hpp"
#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error source_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

constexpr std::uint64_t kClockHz = 90000;

ScalePlan plan_for(const RenditionSpec& spec, std::uint32_t src_w, std::uint32_t src_h) {
    Preset preset;
    if (spec.width) preset.width = spec.width;
    if (spec.height) preset.height = spec.height;
    preset.fit_mode = spec.fit_mode;
    return compute_scale_plan(preset, src_w, src_h);
}

} // namespace

SourceTranscoder::SourceTranscoder(std::vector<RenditionSpec> renditions, std::uint32_t fps)
    : specs_(std::move(renditions)), fps_(std::max<std::uint32_t>(fps, 1)) {}

SourceTranscoder::~SourceTranscoder() = default;

core::Result<void> SourceTranscoder::start() {
    if (started_) return {};
    if (specs_.empty()) return source_error("no renditions configured");
    if (auto r = video_decoder_.initialize(); !r) return r.error();
    for (auto& spec : specs_) {
        auto rendition = std::make_unique<Rendition>();
        rendition->spec = spec;
        renditions_.push_back(std::move(rendition));
    }
    started_ = true;
    return {};
}

core::Result<void> SourceTranscoder::ensure_video(Rendition& rendition, std::uint32_t src_w,
                                                  std::uint32_t src_h) {
    if (rendition.video_open) return {};
    const ScalePlan plan = plan_for(rendition.spec, src_w, src_h);
    const auto config = build_h264_config(plan.out_w, plan.out_h, fps_,
                                          rendition.spec.video_bitrate, rendition.spec.gop, 1);
    if (auto r = rendition.video_encoder.open(config); !r) return r.error();
    rendition.video_open = true;
    return {};
}

core::Result<void> SourceTranscoder::on_video(std::span<const std::byte> annexb,
                                              std::int64_t pts_90k, std::int64_t dts_90k,
                                              bool /*keyframe*/) {
    if (!started_) return source_error("transcoder not started");

    bool produced = false;
    if (auto r = video_decoder_.decode(annexb, pts_90k, decoded_, produced); !r) return r.error();
    if (!produced) return {};
    (void)dts_90k; // openh264 realtime output has DTS == PTS

    for (std::size_t i = 0; i < renditions_.size(); ++i) {
        auto& rendition = *renditions_[i];
        if (auto r = ensure_video(rendition, decoded_.width, decoded_.height); !r) return r.error();

        const ScalePlan plan = plan_for(rendition.spec, decoded_.width, decoded_.height);
        if (auto r = rendition.scaler.scale(decoded_, plan, rendition.scaled); !r) return r.error();

        std::vector<EncodedAccessUnit> encoded;
        if (auto r = rendition.video_encoder.encode(rendition.scaled, encoded); !r) return r.error();
        if (video_output_)
            for (const auto& au : encoded) video_output_(i, au);
    }
    return {};
}

core::Result<void> SourceTranscoder::on_audio(std::span<const std::byte> adts,
                                              std::int64_t pts_90k) {
    if (!started_) return source_error("transcoder not started");
    if (!audio_configured_) {
        if (auto r = audio_decoder_.configure_adts(); !r) return r.error();
        audio_configured_ = true;
    }

    bool produced = false;
    if (auto r = audio_decoder_.decode(adts, pcm_, produced); !r) return r.error();
    if (!produced) return {};

    for (std::size_t i = 0; i < renditions_.size(); ++i) {
        auto& rendition = *renditions_[i];
        if (!rendition.audio_open) {
            Preset preset;
            preset.audio_bitrate = rendition.spec.audio_bitrate;
            const auto params = build_aac_param_set(preset, AacQualityOptions{}, pcm_.sample_rate,
                                                    pcm_.channels);
            if (auto r = rendition.audio_encoder.open(params); !r) return r.error();
            rendition.audio_open = true;
        }
        if (!rendition.audio_base_set) {
            rendition.audio_base_pts_90k = pts_90k;
            rendition.audio_base_set = true;
        }

        std::vector<EncodedAudioFrame> frames;
        if (auto r = rendition.audio_encoder.encode(pcm_, frames); !r) return r.error();
        const std::uint32_t rate = rendition.audio_encoder.sample_rate();
        for (const auto& frame : frames) {
            const std::int64_t out_pts =
                rendition.audio_base_pts_90k +
                static_cast<std::int64_t>(rendition.audio_samples * kClockHz / std::max(rate, 1u));
            rendition.audio_samples += frame.samples_per_channel;
            if (audio_output_) audio_output_(i, frame, out_pts);
        }
    }
    return {};
}

} // namespace rtmp_server::transcoding::native
