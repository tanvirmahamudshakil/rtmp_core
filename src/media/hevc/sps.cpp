#include "rtmp_server/media/hevc/sps.hpp"

#include <vector>

#include "rtmp_server/media/bitstream/bit_reader.hpp"

namespace rtmp_server::media::hevc {

namespace {

core::Error malformed(std::string_view what) {
    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol, what);
}

// profile_tier_level (ITU-T H.265 7.3.3). Nothing downstream needs the
// profile itself yet, so this only advances the reader past it.
void skip_profile_tier_level(bitstream::BitReader& reader, std::uint32_t max_sub_layers_minus1) {
    reader.skip(8);  // general_profile_space(2) tier(1) profile_idc(5)
    reader.skip(32); // general_profile_compatibility_flag[32]
    reader.skip(48); // progressive/interlaced/non-packed/frame-only + 43 reserved + inbld
    reader.skip(8);  // general_level_idc

    std::vector<bool> profile_present(max_sub_layers_minus1, false);
    std::vector<bool> level_present(max_sub_layers_minus1, false);
    for (std::uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        profile_present[i] = reader.flag();
        level_present[i] = reader.flag();
    }
    if (max_sub_layers_minus1 > 0) {
        for (std::uint32_t i = max_sub_layers_minus1; i < 8; ++i) reader.skip(2); // reserved_zero_2bits
    }
    for (std::uint32_t i = 0; i < max_sub_layers_minus1 && !reader.overrun(); ++i) {
        if (profile_present[i]) reader.skip(88);
        if (level_present[i]) reader.skip(8);
    }
}

} // namespace

core::Result<VideoDimensions> parse_sps_dimensions(std::span<const std::byte> sps_nal) {
    if (sps_nal.size() < 2) return malformed("empty or truncated HEVC SPS NAL");
    const auto nal_type = static_cast<std::uint8_t>((static_cast<std::uint8_t>(sps_nal[0]) >> 1) & 0x3F);
    if (nal_type != kNalTypeSps) return malformed("NAL unit is not an HEVC SPS");

    const std::vector<std::byte> rbsp = bitstream::unescape_rbsp(sps_nal.subspan(2));
    bitstream::BitReader reader(rbsp);

    reader.skip(4); // sps_video_parameter_set_id
    const std::uint32_t max_sub_layers_minus1 = reader.u(3);
    reader.skip(1); // sps_temporal_id_nesting_flag
    skip_profile_tier_level(reader, max_sub_layers_minus1);

    (void)reader.ue(); // sps_seq_parameter_set_id
    const std::uint32_t chroma_format_idc = reader.ue();
    if (chroma_format_idc == 3) reader.skip(1); // separate_colour_plane_flag

    const std::uint32_t coded_width = reader.ue();  // pic_width_in_luma_samples
    const std::uint32_t coded_height = reader.ue(); // pic_height_in_luma_samples

    std::uint32_t win_left = 0;
    std::uint32_t win_right = 0;
    std::uint32_t win_top = 0;
    std::uint32_t win_bottom = 0;
    if (reader.flag()) { // conformance_window_flag
        win_left = reader.ue();
        win_right = reader.ue();
        win_top = reader.ue();
        win_bottom = reader.ue();
    }

    if (reader.overrun()) return malformed("truncated HEVC SPS");
    if (coded_width == 0 || coded_height == 0) return malformed("HEVC SPS declares a zero-sized picture");

    // The conformance window is expressed in chroma units (H.265 7.4.3.2.1).
    std::uint32_t sub_width_c = 1;
    std::uint32_t sub_height_c = 1;
    if (chroma_format_idc == 1) {
        sub_width_c = 2;
        sub_height_c = 2;
    } else if (chroma_format_idc == 2) {
        sub_width_c = 2;
    }

    const std::uint64_t cropped_x = static_cast<std::uint64_t>(sub_width_c) * (win_left + win_right);
    const std::uint64_t cropped_y = static_cast<std::uint64_t>(sub_height_c) * (win_top + win_bottom);
    if (cropped_x >= coded_width || cropped_y >= coded_height) {
        return malformed("HEVC SPS conformance window exceeds the coded picture");
    }

    VideoDimensions dimensions;
    dimensions.width = static_cast<std::uint32_t>(coded_width - cropped_x);
    dimensions.height = static_cast<std::uint32_t>(coded_height - cropped_y);
    return dimensions;
}

core::Result<VideoDimensions> parse_dimensions(const HevcDecoderConfig& config) {
    if (config.sps.empty()) return malformed("HEVC decoder config carries no SPS");
    return parse_sps_dimensions(config.sps.front());
}

} // namespace rtmp_server::media::hevc
