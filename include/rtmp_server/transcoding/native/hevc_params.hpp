#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding::native {

// Tunables that control the "same quality, lower bitrate" behaviour of the
// x265 backend. Defaults are chosen for live streaming: a constant-quality
// target (CRF) constrained by a VBV buffer so the muxer/CDN never sees a burst
// larger than the advertised bitrate, plus the psychovisual and adaptive-quant
// tools that let HEVC hold the same perceived quality at a materially lower
// bitrate than a fixed-ABR H.264 ladder.
struct HevcQualityOptions {
    // x265 preset name (ultrafast..placebo). "medium" is roughly realtime for
    // 720p on a modern core; drop to "faster"/"veryfast" for higher density,
    // raise to "slow" for more compression per bit when CPU allows.
    std::string preset = "medium";

    // x265 tune. Empty = none. For live low-latency use "zerolatency"; for
    // fidelity-sensitive content "grain" or "ssim" trade differently. Leaving
    // it empty keeps x265's psy defaults, which favour perceived sharpness.
    std::string tune = "zerolatency";

    // Constant Rate Factor: the quality anchor. Lower = better quality, larger
    // files. 23 is x265's visually-transparent-ish default; 20-24 is the useful
    // live range. When constrain_to_bitrate is true the preset's video_bitrate
    // becomes the VBV ceiling (maxrate) so quality floats *up to* that cap but
    // spends fewer bits on easy scenes — this is what yields the bitrate saving.
    double crf = 23.0;

    // When true, use CRF + VBV (preset.video_bitrate as maxrate). When false,
    // use strict two-pass-style ABR at exactly preset.video_bitrate.
    bool constrain_to_bitrate = true;

    // Adaptive quantisation mode (0..4). 3 = auto-variance with dark-scene and
    // edge bias; keeps detail in shadows and flat gradients where banding shows.
    int aq_mode = 3;
    double aq_strength = 1.0;

    // Psychovisual rate-distortion: preserves apparent detail/energy the eye
    // notices even when it is not strictly rate-optimal. The single biggest
    // "looks the same at lower bitrate" lever after AQ.
    double psy_rd = 2.0;
    double psy_rdoq = 1.0;

    // Motion/reference depth. More B-frames and references cost encode time but
    // buy compression; 4 B-frames + 4 refs is a good live/quality balance.
    int bframes = 4;
    int ref_frames = 4;

    // Emit VPS/SPS/PPS before every IDR so each keyframe (and therefore each
    // HLS/TS segment) is independently decodable when a player joins mid-stream.
    bool repeat_headers = true;

    // Encoder worker threads. 0 = let x265 pick from the frame size; set a hard
    // cap to bound CPU per job when many jobs share a box.
    int frame_threads = 0;
    int pools_threads = 0; // 0 = auto
};

// The resolved, backend-agnostic description of one HEVC encode, ready to be
// handed to x265_param_default_preset + x265_param_parse.
struct HevcParamSet {
    std::string preset;
    std::string tune;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_num = 30;
    std::uint32_t fps_den = 1;
    std::uint32_t keyint = 60;     // max GOP length in frames
    std::uint32_t min_keyint = 60; // fixed GOP for segment alignment
    // Ordered x265_param_parse(key, value) pairs applied after the preset/tune.
    std::vector<std::pair<std::string, std::string>> options;
};

// Builds the x265 parameter set for `preset` at the given output geometry and
// frame rate. Pure: no x265 dependency, so the mapping is unit-testable. The
// keyframe interval comes from preset.keyframe_interval when set (so segments
// align to a known GOP); otherwise it defaults to 2 seconds of frames.
[[nodiscard]] HevcParamSet build_hevc_param_set(const Preset& preset, const HevcQualityOptions& quality,
                                                std::uint32_t out_w, std::uint32_t out_h,
                                                std::uint32_t fps_num, std::uint32_t fps_den);

} // namespace rtmp_server::transcoding::native
