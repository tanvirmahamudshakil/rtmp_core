#include "rtmp_server/transcoding/native/aac_params.hpp"

#include <algorithm>

namespace rtmp_server::transcoding::native {

AacParamSet build_aac_param_set(const Preset& preset, const AacQualityOptions& quality,
                                std::uint32_t src_sample_rate, std::uint32_t src_channels) {
    AacParamSet set;
    set.sample_rate = src_sample_rate > 0 ? src_sample_rate : 44100;
    set.channels = std::clamp<std::uint32_t>(src_channels, 1, 2);
    set.bitrate = static_cast<std::uint32_t>(std::max<std::uint64_t>(8'000, preset.audio_bitrate));
    set.afterburner = quality.afterburner;

    AacProfile profile = quality.profile;
    // Auto-pick HE-AAC for low-bitrate ladders when the caller allows it and
    // left the profile at the LC default.
    if (quality.allow_auto_profile && profile == AacProfile::LowComplexity &&
        quality.auto_he_below_bps > 0 && set.bitrate < quality.auto_he_below_bps) {
        profile = set.channels == 2 ? AacProfile::HighEfficiencyV2 : AacProfile::HighEfficiency;
    }
    // Parametric stereo (HE-AACv2) requires two channels; fall back otherwise.
    if (profile == AacProfile::HighEfficiencyV2 && set.channels != 2) {
        profile = AacProfile::HighEfficiency;
    }
    set.profile = profile;
    return set;
}

} // namespace rtmp_server::transcoding::native
