#include "rtmp_server/transcoding/native/codec_tags.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "rtmp_server/transcoding/native/aac_params.hpp"
#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding::native {

namespace {

struct LevelLimit {
    std::uint32_t level_idc; // 10 * level, e.g. 3.1 -> 31
    std::uint64_t max_frame_mbs;
    std::uint64_t max_mbs_per_second;
};

// ISO/IEC 14496-10 Table A-1. Only the levels a live ladder can plausibly
// land on are listed; anything past the last entry is declared as that entry
// (5.2 is the practical ceiling for streaming H.264).
constexpr std::array<LevelLimit, 11> kLevels = {{
    {30, 1620, 40500},      // 3.0  — up to 720x576@25
    {31, 3600, 108000},     // 3.1  — up to 1280x720@30
    {32, 5120, 216000},     // 3.2  — up to 1280x720@60
    {40, 8192, 245760},     // 4.0  — up to 1920x1080@30
    {41, 8192, 245760},     // 4.1  — same limits, higher bitrate ceiling
    {42, 8704, 522240},     // 4.2  — up to 1920x1080@60
    {50, 22080, 589824},    // 5.0
    {51, 36864, 983040},    // 5.1  — up to 4096x2160@30
    {52, 36864, 2073600},   // 5.2  — up to 4096x2160@60
    {60, 139264, 4177920},  // 6.0
    {62, 139264, 16711680}, // 6.2
}};

// Level declared for a rendition whose output geometry is only resolved once
// the source's first frame decodes. Over-declaring is safe; under-declaring
// is a decode failure.
constexpr std::uint32_t kUnknownGeometryLevel = 51;

} // namespace

std::uint32_t h264_level_idc(std::uint32_t width, std::uint32_t height, std::uint32_t fps) {
    if (width == 0 || height == 0) return kUnknownGeometryLevel;

    const std::uint64_t mb_width = (static_cast<std::uint64_t>(width) + 15) / 16;
    const std::uint64_t mb_height = (static_cast<std::uint64_t>(height) + 15) / 16;
    const std::uint64_t frame_mbs = mb_width * mb_height;
    const std::uint64_t rate = std::max<std::uint32_t>(fps, 1);
    const std::uint64_t mbs_per_second = frame_mbs * rate;

    for (const auto& level : kLevels) {
        if (frame_mbs <= level.max_frame_mbs && mbs_per_second <= level.max_mbs_per_second) {
            return level.level_idc;
        }
    }
    return kLevels.back().level_idc;
}

std::string hls_codecs_attribute(std::uint32_t width, std::uint32_t height, std::uint32_t fps,
                                 std::uint32_t audio_bitrate) {
    // H264Encoder::open applies the "high" profile (H264EncoderConfig::profile
    // defaults to 2), so profile_idc is 0x64 and no constraint_set flag is
    // set. The trailing byte is the level.
    const auto level = h264_level_idc(width, height, fps);
    char video[16];
    std::snprintf(video, sizeof(video), "avc1.6400%02X", static_cast<unsigned>(level));

    Preset preset;
    preset.audio_bitrate = audio_bitrate;
    // Two channels: see the header's note on why the source's real channel
    // count is not knowable at master-playlist time.
    const auto audio = build_aac_param_set(preset, AacQualityOptions{}, 48'000, 2);
    char audio_tag[16];
    std::snprintf(audio_tag, sizeof(audio_tag), "mp4a.40.%d", audio.audio_object_type());

    return std::string(video) + "," + audio_tag;
}

} // namespace rtmp_server::transcoding::native
