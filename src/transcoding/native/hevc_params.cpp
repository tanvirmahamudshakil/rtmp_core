#include "rtmp_server/transcoding/native/hevc_params.hpp"

#include <algorithm>
#include <string>

namespace rtmp_server::transcoding::native {

namespace {

std::string to_str(double value) {
    // x265 parses locale-independent decimal; keep it short and stable.
    std::string s = std::to_string(value);
    // Trim trailing zeros for readability of generated params.
    if (s.find('.') != std::string::npos) {
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// Bits/sec -> kbit/sec, which is what x265's bitrate/vbv params expect.
std::uint32_t to_kbit(std::uint64_t bits_per_sec) {
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(1, bits_per_sec / 1000));
}

} // namespace

HevcParamSet build_hevc_param_set(const Preset& preset, const HevcQualityOptions& quality,
                                  std::uint32_t out_w, std::uint32_t out_h, std::uint32_t fps_num,
                                  std::uint32_t fps_den) {
    HevcParamSet set;
    set.preset = quality.preset;
    set.tune = quality.tune;
    set.width = std::max<std::uint32_t>(out_w, 2);
    set.height = std::max<std::uint32_t>(out_h, 2);
    set.fps_num = std::max<std::uint32_t>(fps_num, 1);
    set.fps_den = std::max<std::uint32_t>(fps_den, 1);

    const std::uint32_t fps_round =
        std::max<std::uint32_t>(1, (set.fps_num + set.fps_den / 2) / set.fps_den);
    const std::uint32_t keyint =
        preset.keyframe_interval.value_or(fps_round * 2); // default 2s GOP
    set.keyint = std::max<std::uint32_t>(keyint, 1);
    set.min_keyint = set.keyint; // fixed GOP so HLS segments align to keyframes

    auto& opts = set.options;
    const auto add = [&opts](std::string key, std::string value) {
        opts.emplace_back(std::move(key), std::move(value));
    };

    // --- Rate control: the core of "same quality, lower bitrate". ---
    const std::uint32_t kbit = to_kbit(preset.video_bitrate);
    if (quality.constrain_to_bitrate) {
        // CRF anchors perceived quality; VBV caps peak so easy scenes spend
        // fewer bits than a fixed-ABR ladder would, while hard scenes stay
        // under the advertised ceiling.
        add("crf", to_str(quality.crf));
        add("vbv-maxrate", std::to_string(kbit));
        add("vbv-bufsize", std::to_string(kbit * 2));
    } else {
        add("bitrate", std::to_string(kbit));
        add("vbv-maxrate", std::to_string(kbit));
        add("vbv-bufsize", std::to_string(kbit * 2));
    }

    // --- Perceptual quality tools. ---
    add("aq-mode", std::to_string(quality.aq_mode));
    add("aq-strength", to_str(quality.aq_strength));
    add("psy-rd", to_str(quality.psy_rd));
    add("psy-rdoq", to_str(quality.psy_rdoq));

    // --- Motion / reference depth. ---
    add("bframes", std::to_string(std::max(0, quality.bframes)));
    add("ref", std::to_string(std::max(1, quality.ref_frames)));
    add("b-adapt", "2");
    add("weightp", "1");
    add("weightb", "1");
    add("rc-lookahead", std::to_string(std::max<std::uint32_t>(set.keyint, 20)));

    // --- GOP structure for HLS/TS segment alignment. ---
    add("keyint", std::to_string(set.keyint));
    add("min-keyint", std::to_string(set.min_keyint));
    add("scenecut", "0"); // fixed cadence: no extra IDRs mid-GOP
    if (quality.repeat_headers) add("repeat-headers", "1");

    // Annex B start codes so access units feed the TS muxer directly.
    add("annexb", "1");
    add("aud", "1");
    add("info", "0");

    if (quality.frame_threads > 0) add("frame-threads", std::to_string(quality.frame_threads));
    if (quality.pools_threads > 0) add("pools", std::to_string(quality.pools_threads));

    return set;
}

} // namespace rtmp_server::transcoding::native
