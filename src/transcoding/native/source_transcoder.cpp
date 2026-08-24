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

SourceTranscoder::SourceTranscoder(std::vector<RenditionSpec> renditions, std::uint32_t fps,
                                   SourceVideoCodec video_codec)
    : specs_(std::move(renditions)), fps_(std::max<std::uint32_t>(fps, 1)),
      video_codec_(video_codec) {
    if (video_codec_ == SourceVideoCodec::Hevc) {
        video_decoder_.emplace<HevcDecoder>();
    }
    // else: variant already default-constructed to H264Decoder.
}

SourceTranscoder::~SourceTranscoder() = default;

core::Result<void> SourceTranscoder::start() {
    if (started_) return {};
    if (specs_.empty()) return source_error("no renditions configured");
    auto init_result = std::visit([](auto& decoder) { return decoder.initialize(); }, video_decoder_);
    if (!init_result) return init_result.error();
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
    // A resolution-tiered baseline alone left a low-res job (720p and below
    // -- most source-transcode jobs, which are typically one or two low-
    // bitrate renditions) pinned to 1-2 encoder threads regardless of how
    // many cores the box actually has, since desired was never more than
    // that below 1080p. fair_share spreads the box's cores evenly across
    // however many renditions this job has instead, so a single-rendition
    // job on a 24-core box gets a real slice of it rather than the
    // resolution tier's fixed floor; higher tiers still get at least their
    // own floor on a box with more renditions than cores to go around.
    const std::size_t fair_share =
        std::max<std::size_t>(1, core_budget / std::max<std::size_t>(renditions_.size(), 1));
    for (auto& rendition : renditions_) {
        const auto pixels =
            static_cast<std::uint64_t>(rendition->spec.width) * rendition->spec.height;
        std::size_t desired = fair_share;
        if (pixels >= 3840ULL * 2160ULL) {
            desired = std::max(fair_share, std::size_t{8});
        } else if (pixels >= 1920ULL * 1080ULL) {
            desired = std::max(fair_share, std::size_t{4});
        } else if (pixels >= 1280ULL * 720ULL) {
            desired = std::max(fair_share, std::size_t{2});
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
    // std::visit dispatches to whichever decoder this job was constructed
    // with (H264Decoder or HevcDecoder); both expose the identical
    // decode(annexb, pts_90k, out, produced) shape, so the H.264 branch below
    // is byte-for-byte the same call it always was.
    auto decode_result = std::visit(
        [&](auto& decoder) { return decoder.decode(annexb, pts_90k, decoded_, produced); },
        video_decoder_);
    if (!decode_result) return decode_result.error();
    if (!produced) return {};
    (void)dts_90k; // realtime decoder output has DTS == PTS

    // Source jobs have an explicit output frame rate. Decode every source
    // frame (reference pictures are still required), but do not feed a 50/60
    // fps source into a 30 fps encoder. Without this gate the encoder's target
    // bitrate was consumed once per configured-frame interval while frames
    // arrived twice as fast, doubling real egress.
    const auto output_interval_90k =
        std::max<std::int64_t>(1, 90'000 / static_cast<std::int64_t>(fps_));
    if (video_clock_set_) {
        if (awaiting_video_reanchor_) {
            last_input_video_pts_90k_ = decoded_.pts_90k;
            next_input_video_pts_90k_ = decoded_.pts_90k + output_interval_90k;
            next_output_video_pts_90k_ += output_interval_90k;
            consecutive_backward_drops_ = 0;
            awaiting_video_reanchor_ = false;
        } else {
            const auto input_delta = decoded_.pts_90k - last_input_video_pts_90k_;
            constexpr std::int64_t kBackwardDiscontinuity90k = 5 * 90'000;
            if (input_delta <= 0 && input_delta > -kBackwardDiscontinuity90k) {
                // Tolerate brief backward jitter (ordinary source noise), but
                // don't let it freeze video for the full 5s tolerance: many
                // upstream IPTV panels reset timestamps at their own internal
                // segment boundaries without ever setting EXT-X-DISCONTINUITY,
                // so mark_discontinuity()'s explicit reset (hls_source_puller.cpp)
                // never fires for those and this gate would otherwise silently
                // drop every frame for up to 5s each time -- audio has no
                // equivalent gate, so it kept playing throughout, which is what
                // read as "video freezes constantly, audio keeps going". A real
                // reset (source's internal segment boundary) persists for many
                // consecutive frames; ordinary decode-level noise (duplicate or
                // slightly-reordered timestamps) self-corrects within a frame or
                // two. So there's little upside to waiting anywhere near 0.5s to
                // decide which one this is -- a handful of frames (well under
                // 200ms at any realistic fps) is enough to tell them apart, and
                // recovering that fast keeps a real reset from reading as a
                // visible freeze at all.
                constexpr std::int64_t kMaxToleratedDrops = 3;
                if (++consecutive_backward_drops_ < kMaxToleratedDrops) {
                    return {};
                }
            }
            consecutive_backward_drops_ = 0;
            last_input_video_pts_90k_ = decoded_.pts_90k;

            if (input_delta <= 0 || input_delta > kBackwardDiscontinuity90k) {
                // Start a new sampling phase after a real timestamp discontinuity,
                // while keeping the generated output clock monotonic.
                next_input_video_pts_90k_ = decoded_.pts_90k + output_interval_90k;
            } else {
                // Select against an absolute input-PTS deadline, with a small
                // tolerance for MPEG-TS's millisecond-rounded timestamps. At
                // 30fps those timestamps commonly alternate 2970/3060 ticks. A
                // delta accumulator starting from zero treated every 2970-tick
                // frame as "too early" and emitted only 2/3 of a genuine 30fps
                // source, while audio kept all of its samples and ran far ahead.
                // Absolute deadlines preserve all ~30fps frames and still select
                // every other frame from a 60fps source.
                const auto jitter_tolerance_90k =
                    std::max<std::int64_t>(1, output_interval_90k / 20);
                if (decoded_.pts_90k + jitter_tolerance_90k < next_input_video_pts_90k_)
                    return {};
                next_input_video_pts_90k_ += output_interval_90k;
                if (next_input_video_pts_90k_ <= decoded_.pts_90k + jitter_tolerance_90k) {
                    // A low-frame-rate source can cross more than one nominal
                    // deadline between pictures. Never duplicate a picture to
                    // catch up; schedule the next decision from this one.
                    next_input_video_pts_90k_ = decoded_.pts_90k + output_interval_90k;
                }
            }
            next_output_video_pts_90k_ += output_interval_90k;
        }
    } else {
        video_clock_set_ = true;
        // Video's own output clock just starts from its own raw source PTS
        // -- it never needs to agree with any other stream's coordinate
        // space, only be internally consistent with its own later frames
        // (the delta comparisons above). on_audio anchors to *this* value
        // directly (see its comment) rather than to any shared "program
        // start", so audio ends up in video's coordinate space automatically
        // regardless of what video's own base happens to be.
        next_output_video_pts_90k_ = decoded_.pts_90k;
        next_input_video_pts_90k_ = decoded_.pts_90k + output_interval_90k;
        last_input_video_pts_90k_ = decoded_.pts_90k;
        consecutive_backward_drops_ = 0;
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
                                              std::int64_t /*pts_90k*/) {
    if (!started_) return source_error("transcoder not started");
    if (!audio_configured_) {
        if (auto r = audio_decoder_.configure_adts(); !r) return r.error();
        audio_configured_ = true;
    }

    bool produced = false;
    if (auto r = audio_decoder_.decode(adts, pcm_, produced); !r) return r.error();
    if (!produced) return {};

    // Audio's anchor (below) is video's current output position -- there is
    // no valid fallback for "video hasn't decoded a frame yet": video's
    // clock is in raw source-PTS units (arbitrarily large, whatever the
    // source's own absolute clock says), so any placeholder value picked
    // here (0, or audio's own raw PTS) would be in a completely different
    // coordinate space once video does start, producing exactly the kind of
    // massive misalignment this anchor exists to prevent. Simplest correct
    // behavior: audio frames that arrive before video's first one are
    // dropped (still decoded, to keep the decoder's internal state warm --
    // just not encoded/emitted) until video establishes a real position to
    // anchor to. In practice video locks onto its first keyframe within a
    // frame or two, so this costs at most a handful of audio frames.
    if (!video_clock_set_ || awaiting_video_reanchor_) return {};

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
            // Anchor to video's *current output position*, not to this
            // frame's own raw incoming pts_90k. Measured on real sources:
            // the source's own audio and video PES streams can carry PTS
            // values tens of seconds apart with no discontinuity between
            // them -- an upstream muxing quirk, not something libfdk-aac
            // being slow to lock on explains (it produces output within a
            // frame or two of being fed valid ADTS). Trusting audio's own
            // raw PTS for the anchor just inherited that gap verbatim into
            // the output, which was severe enough that players dropped the
            // audio track rather than play it desynced. Tying the anchor to
            // wherever video's output clock already is sidesteps the
            // source's PTS relationship entirely -- audio starts exactly
            // when it's decoded, at whatever point in the stream video is
            // currently showing, and paces itself from there purely by its
            // own sample count (below), immune to whatever the source's PTS
            // says. on_audio already returned above if video has not produced
            // a frame yet, so this anchor is always in video's real clock
            // domain and never needs an incompatible zero fallback.
            rendition.audio_base_pts_90k = next_output_video_pts_90k_;
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

        // Never hard-reset this sample clock to the most recently processed
        // video callback. Real TS muxes may place audio PES packets seconds
        // ahead of video in packet order even when their timestamps are
        // valid. Re-anchoring to callback order made AAC PTS jump backwards
        // by seconds every few moments. This exact sample count stays
        // monotonic until an explicit source discontinuity, where
        // mark_discontinuity() starts a fresh clock and HLS emits the matching
        // EXT-X-DISCONTINUITY marker.
    }
    return {};
}

} // namespace rtmp_server::transcoding::native
