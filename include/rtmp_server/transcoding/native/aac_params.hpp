#pragma once

#include <cstdint>

#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding::native {

// AAC audio-object type selection. AAC-LC is universal; HE-AAC (SBR) and
// HE-AACv2 (SBR+PS) hold quality at much lower bitrates for mobile ladders, at
// the cost of a doubled encoder frame length and slightly higher latency.
enum class AacProfile {
    LowComplexity, // AOT 2  — AAC-LC
    HighEfficiency, // AOT 5  — HE-AAC / AAC+
    HighEfficiencyV2, // AOT 29 — HE-AACv2 (stereo only)
};

// Tunables for the libfdk-aac encoder, independent of source format.
struct AacQualityOptions {
    AacProfile profile = AacProfile::LowComplexity;
    // libfdk "afterburner": extra bit-distribution search that measurably
    // improves quality at the same bitrate for a small CPU cost. On by default.
    bool afterburner = true;
    // Auto-select HE-AAC below this per-stream bitrate when profile is left at
    // LowComplexity and allow_auto_profile is set. 0 disables auto-selection.
    std::uint32_t auto_he_below_bps = 64'000;
    bool allow_auto_profile = true;
};

// The resolved encoder description, ready for aacEncoder_SetParam.
struct AacParamSet {
    AacProfile profile = AacProfile::LowComplexity;
    std::uint32_t sample_rate = 44100; // matches the source; no resampling
    std::uint32_t channels = 2;
    std::uint32_t bitrate = 128'000; // bits/sec
    bool afterburner = true;

    [[nodiscard]] int audio_object_type() const noexcept {
        switch (profile) {
            case AacProfile::LowComplexity: return 2;
            case AacProfile::HighEfficiency: return 5;
            case AacProfile::HighEfficiencyV2: return 29;
        }
        return 2;
    }
};

// Builds the AAC encoder parameters for `preset` given the decoded source's
// sample rate and channel count. Pure and testable: no libfdk dependency.
// HE-AACv2 is downgraded to HE-AAC for non-stereo sources (PS needs 2 channels).
[[nodiscard]] AacParamSet build_aac_param_set(const Preset& preset, const AacQualityOptions& quality,
                                              std::uint32_t src_sample_rate,
                                              std::uint32_t src_channels);

} // namespace rtmp_server::transcoding::native
