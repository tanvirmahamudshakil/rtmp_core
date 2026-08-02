#include "rtmp_server/transcoding/native/source_transcoder.hpp"

#include <algorithm>
#include <thread>

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
    // Give large encoders enough slices to consume the cores that become idle
    // as smaller renditions finish their work for this frame. Treating encoder
    // thread counts as an additive fixed budget left the 1080p barrier with
    // only two cores even though its 720p/360p peers finish much earlier.
    const auto hardware = std::thread::hardware_concurrency();
    const std::size_t core_budget = static_cast<std::size_t>(hardware > 0 ? hardware : 1);
    for (auto& rendition : renditions_) {
        const auto pixels =
            static_cast<std::uint64_t>(rendition->spec.width) * rendition->spec.height;
        std::size_t desired = 1;
        if (pixels >= 3840ULL * 2160ULL) {
            desired = 8;
        } else if (pixels >= 1920ULL * 1080ULL) {
            desired = 4;
        } else if (pixels >= 1280ULL * 720ULL) {
            desired = 2;
        }
        rendition->video_threads =
            static_cast<std::uint32_t>(std::clamp<std::size_t>(desired, 1, core_budget));
    }

    // Fan separate renditions out in parallel. Internal encoder threads use
    // only the spare part of the same core budget calculated above.
    const std::size_t pool_size =
        std::min(renditions_.size(), core_budget);
    if (pool_size > 1) render_pool_ = std::make_unique<core::ThreadPool>(pool_size);
    started_ = true;
    return {};
}

core::Result<void> SourceTranscoder::ensure_video(Rendition& rendition, std::uint32_t src_w,
                                                  std::uint32_t src_h) {
    if (rendition.video_open) return {};
    const ScalePlan plan = plan_for(rendition.spec, src_w, src_h);
    auto config = build_h264_config(plan.out_w, plan.out_h, fps_,
                                    rendition.spec.video_bitrate, rendition.spec.gop,
                                    rendition.video_threads);
    // OpenH264 RC frame skipping can suppress an IDR along with many adjacent
    // pictures. That moves otherwise-aligned rendition boundaries by seconds
    // and starves live-edge players even though the remaining PTS are valid.
    // Keep a stable cadence; realtime overload is handled through parallel
    // encoding, not an encoder-local decision that the segmenter cannot see.
    config.allow_frame_skip = false;
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

    // Source jobs have an explicit output frame rate. Decode every source
    // frame (reference pictures are still required), but do not feed a 50/60
    // fps source into a 30 fps encoder. Without this gate the encoder's target
    // bitrate was consumed once per configured-frame interval while frames
    // arrived twice as fast, doubling real egress.
    const auto output_interval_90k =
        std::max<std::int64_t>(1, 90'000 / static_cast<std::int64_t>(fps_));
    if (video_clock_set_) {
        const auto input_delta = decoded_.pts_90k - last_input_video_pts_90k_;
        constexpr std::int64_t kBackwardDiscontinuity90k = 5 * 90'000;
        if (input_delta <= 0 && input_delta > -kBackwardDiscontinuity90k) return {};
        last_input_video_pts_90k_ = decoded_.pts_90k;

        if (input_delta <= 0 || input_delta > kBackwardDiscontinuity90k) {
            // Start a new sampling phase after a real timestamp discontinuity,
            // while keeping the generated output clock monotonic.
            frame_selection_accumulator_ = 0;
        } else {
            // Accumulate elapsed source time instead of comparing one rounded
            // delta with output_interval_90k. MPEG-TS sources often express
            // 60 fps as alternating millisecond-rounded deltas (for example
            // 1440/1530 ticks); a strict 3000-tick comparison selected every
            // third frame and turned 60 fps into 20 fps. This phase
            // accumulator preserves the requested average for any input FPS.
            frame_selection_accumulator_ +=
                input_delta * static_cast<std::int64_t>(fps_);
            if (frame_selection_accumulator_ < static_cast<std::int64_t>(kClockHz)) return {};
            frame_selection_accumulator_ %= static_cast<std::int64_t>(kClockHz);
        }
        next_output_video_pts_90k_ += output_interval_90k;
    } else {
        video_clock_set_ = true;
        next_output_video_pts_90k_ = decoded_.pts_90k;
        last_input_video_pts_90k_ = decoded_.pts_90k;
    }
    // Output H.264 has no B-frames. Give every accepted frame an exact,
    // monotonic output cadence so MPEG-TS DTS can never move backwards even
    // when an upstream HLS source reconnects or resets its timestamp base.
    decoded_.pts_90k = next_output_video_pts_90k_;

    // Each rendition writes only to its own Rendition (scaler/encoder/scaled
    // buffer) and its own video_output_ index, so running them concurrently
    // is safe: no shared mutable state besides the read-only decoded_ frame.
    std::vector<core::Error> errors(renditions_.size());
    std::vector<bool> failed(renditions_.size(), false);

    auto process = [&](std::size_t i) {
        auto& rendition = *renditions_[i];
        if (auto r = ensure_video(rendition, decoded_.width, decoded_.height); !r) {
            errors[i] = r.error();
            failed[i] = true;
            return;
        }
        const ScalePlan plan = plan_for(rendition.spec, decoded_.width, decoded_.height);
        if (auto r = rendition.scaler.scale(decoded_, plan, rendition.scaled); !r) {
            errors[i] = r.error();
            failed[i] = true;
            return;
        }
        std::vector<EncodedAccessUnit> encoded;
        if (auto r = rendition.video_encoder.encode(rendition.scaled, encoded); !r) {
            errors[i] = r.error();
            failed[i] = true;
            return;
        }
        if (video_output_)
            for (const auto& au : encoded) video_output_(i, au);
    };

    if (render_pool_) {
        render_pool_->parallel_for(renditions_.size(), process);
    } else {
        for (std::size_t i = 0; i < renditions_.size(); ++i) process(i);
    }

    for (std::size_t i = 0; i < renditions_.size(); ++i) {
        if (failed[i]) return errors[i];
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
