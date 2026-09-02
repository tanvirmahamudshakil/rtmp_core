#include "rtmp_server/media/h264/sps.hpp"

#include <array>
#include <vector>

#include "rtmp_server/media/bitstream/bit_reader.hpp"

namespace rtmp_server::media::h264 {

namespace {

core::Error malformed(std::string_view what) {
    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol, what);
}

// Profiles whose SPS carries the chroma_format_idc / bit-depth / scaling-list
// block (ISO/IEC 14496-10 7.3.2.1.1).
bool has_chroma_block(std::uint32_t profile_idc) {
    switch (profile_idc) {
    case 100: case 110: case 122: case 244: case 44:
    case 83:  case 86:  case 118: case 128: case 138:
    case 139: case 134: case 135:
        return true;
    default:
        return false;
    }
}

void skip_scaling_list(bitstream::BitReader& reader, std::uint32_t size) {
    std::int32_t last_scale = 8;
    std::int32_t next_scale = 8;
    for (std::uint32_t i = 0; i < size && !reader.overrun(); ++i) {
        if (next_scale != 0) {
            next_scale = (last_scale + reader.se() + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

} // namespace

core::Result<VideoDimensions> parse_sps_dimensions(std::span<const std::byte> sps_nal) {
    if (sps_nal.empty()) return malformed("empty H.264 SPS NAL");
    const auto nal_type = static_cast<std::uint8_t>(sps_nal[0]) & 0x1F;
    if (nal_type != kNalTypeSps) return malformed("NAL unit is not an H.264 SPS");

    const std::vector<std::byte> rbsp = bitstream::unescape_rbsp(sps_nal.subspan(1));
    bitstream::BitReader reader(rbsp);

    const std::uint32_t profile_idc = reader.u(8);
    reader.skip(8); // constraint_set flags + reserved_zero_2bits
    reader.skip(8); // level_idc
    (void)reader.ue(); // seq_parameter_set_id

    std::uint32_t chroma_format_idc = 1; // 4:2:0 unless the profile says otherwise
    if (has_chroma_block(profile_idc)) {
        chroma_format_idc = reader.ue();
        if (chroma_format_idc == 3) reader.skip(1); // separate_colour_plane_flag
        (void)reader.ue();  // bit_depth_luma_minus8
        (void)reader.ue();  // bit_depth_chroma_minus8
        reader.skip(1);     // qpprime_y_zero_transform_bypass_flag
        if (reader.flag()) { // seq_scaling_matrix_present_flag
            const std::uint32_t list_count = (chroma_format_idc != 3) ? 8u : 12u;
            for (std::uint32_t i = 0; i < list_count && !reader.overrun(); ++i) {
                if (reader.flag()) skip_scaling_list(reader, (i < 6) ? 16u : 64u);
            }
        }
    }

    (void)reader.ue(); // log2_max_frame_num_minus4
    const std::uint32_t pic_order_cnt_type = reader.ue();
    if (pic_order_cnt_type == 0) {
        (void)reader.ue(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        reader.skip(1);    // delta_pic_order_always_zero_flag
        (void)reader.se(); // offset_for_non_ref_pic
        (void)reader.se(); // offset_for_top_to_bottom_field
        const std::uint32_t cycle_length = reader.ue();
        // A corrupt cycle length must not make this loop unbounded; the
        // reader's overrun flag terminates it, and the standard's own upper
        // bound is 255.
        for (std::uint32_t i = 0; i < cycle_length && i < 256 && !reader.overrun(); ++i) {
            (void)reader.se();
        }
    }

    (void)reader.ue(); // max_num_ref_frames
    reader.skip(1);    // gaps_in_frame_num_value_allowed_flag

    const std::uint32_t width_in_mbs = reader.ue() + 1;
    const std::uint32_t height_in_map_units = reader.ue() + 1;
    const bool frame_mbs_only = reader.flag();
    if (!frame_mbs_only) reader.skip(1); // mb_adaptive_frame_field_flag
    reader.skip(1);                      // direct_8x8_inference_flag

    std::uint32_t crop_left = 0;
    std::uint32_t crop_right = 0;
    std::uint32_t crop_top = 0;
    std::uint32_t crop_bottom = 0;
    if (reader.flag()) { // frame_cropping_flag
        crop_left = reader.ue();
        crop_right = reader.ue();
        crop_top = reader.ue();
        crop_bottom = reader.ue();
    }

    if (reader.overrun()) return malformed("truncated H.264 SPS");

    // SubWidthC/SubHeightC per ISO/IEC 14496-10 Table 6-1. Monochrome
    // (chroma_format_idc 0) and 4:4:4 with separate planes crop in luma
    // samples; 4:2:0/4:2:2 crop in chroma units.
    std::uint32_t sub_width_c = 1;
    std::uint32_t sub_height_c = 1;
    if (chroma_format_idc == 1) {
        sub_width_c = 2;
        sub_height_c = 2;
    } else if (chroma_format_idc == 2) {
        sub_width_c = 2;
        sub_height_c = 1;
    }
    const std::uint32_t crop_unit_x = sub_width_c;
    const std::uint32_t crop_unit_y = sub_height_c * (frame_mbs_only ? 1u : 2u);

    const std::uint64_t coded_width = static_cast<std::uint64_t>(width_in_mbs) * 16u;
    const std::uint64_t coded_height =
        static_cast<std::uint64_t>(height_in_map_units) * 16u * (frame_mbs_only ? 1u : 2u);
    const std::uint64_t cropped_x = static_cast<std::uint64_t>(crop_unit_x) * (crop_left + crop_right);
    const std::uint64_t cropped_y = static_cast<std::uint64_t>(crop_unit_y) * (crop_top + crop_bottom);
    if (cropped_x >= coded_width || cropped_y >= coded_height) {
        return malformed("H.264 SPS cropping window exceeds the coded picture");
    }

    VideoDimensions dimensions;
    dimensions.width = static_cast<std::uint32_t>(coded_width - cropped_x);
    dimensions.height = static_cast<std::uint32_t>(coded_height - cropped_y);
    return dimensions;
}

core::Result<VideoDimensions> parse_dimensions(const AvcDecoderConfig& config) {
    if (config.sps.empty()) return malformed("AVC decoder config carries no SPS");
    return parse_sps_dimensions(config.sps.front());
}

} // namespace rtmp_server::media::h264
