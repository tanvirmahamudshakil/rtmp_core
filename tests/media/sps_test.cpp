#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rtmp_server/media/h264/sps.hpp"
#include "rtmp_server/media/hevc/sps.hpp"

using namespace rtmp_server;

namespace {

// Minimal MSB-first bit writer, the inverse of media::bitstream::BitReader.
//
// The parameter sets below are built field by field from the syntax tables in
// ITU-T H.264 7.3.2.1.1 and H.265 7.3.2.2.1 rather than pasted in as magic
// byte strings. A captured SPS would only prove the parser agrees with
// whatever produced that capture; building the bitstream from the spec proves
// it agrees with the spec, and makes each test state the geometry it expects.
class BitWriter {
public:
    void u(std::uint32_t value, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) {
            put_bit((value >> (count - 1 - i)) & 1u);
        }
    }

    void flag(bool value) { put_bit(value ? 1u : 0u); }

    // Unsigned Exp-Golomb.
    void ue(std::uint32_t value) {
        const std::uint64_t code = static_cast<std::uint64_t>(value) + 1u;
        std::uint32_t bits = 0;
        while ((code >> bits) > 1u) ++bits;
        u(0, bits);
        u(static_cast<std::uint32_t>(code), bits + 1);
    }

    [[nodiscard]] std::vector<std::byte> take() {
        // rbsp_trailing_bits: a stop bit, then zero-pad to a byte boundary.
        put_bit(1);
        while (bit_count_ % 8 != 0) put_bit(0);
        return std::move(bytes_);
    }

private:
    void put_bit(std::uint32_t bit) {
        if (bit_count_ % 8 == 0) bytes_.push_back(std::byte{0});
        if (bit != 0) {
            const std::uint32_t shift = 7u - (bit_count_ % 8);
            bytes_.back() |= static_cast<std::byte>(1u << shift);
        }
        ++bit_count_;
    }

    std::vector<std::byte> bytes_;
    std::size_t bit_count_ = 0;
};

// H.264 SPS for a Baseline-profile stream of the given macroblock geometry,
// with an optional frame cropping window in chroma units.
std::vector<std::byte> h264_sps(std::uint32_t width_mbs, std::uint32_t height_map_units,
                                bool frame_mbs_only = true, std::uint32_t crop_right = 0,
                                std::uint32_t crop_bottom = 0) {
    BitWriter w;
    w.u(66, 8);  // profile_idc: Baseline, so no chroma_format_idc block
    w.u(0, 8);   // constraint flags + reserved_zero_2bits
    w.u(30, 8);  // level_idc 3.0
    w.ue(0);     // seq_parameter_set_id
    w.ue(4);     // log2_max_frame_num_minus4
    w.ue(0);     // pic_order_cnt_type
    w.ue(4);     // log2_max_pic_order_cnt_lsb_minus4
    w.ue(1);     // max_num_ref_frames
    w.flag(false); // gaps_in_frame_num_value_allowed_flag
    w.ue(width_mbs - 1);
    w.ue(height_map_units - 1);
    w.flag(frame_mbs_only);
    if (!frame_mbs_only) w.flag(false); // mb_adaptive_frame_field_flag
    w.flag(true);                       // direct_8x8_inference_flag
    const bool cropping = crop_right > 0 || crop_bottom > 0;
    w.flag(cropping);
    if (cropping) {
        w.ue(0);           // crop_left
        w.ue(crop_right);
        w.ue(0);           // crop_top
        w.ue(crop_bottom);
    }
    w.flag(false); // vui_parameters_present_flag

    std::vector<std::byte> nal;
    nal.push_back(std::byte{0x67}); // nal_ref_idc 3, nal_unit_type 7 (SPS)
    const auto rbsp = w.take();
    nal.insert(nal.end(), rbsp.begin(), rbsp.end());
    return nal;
}

std::vector<std::byte> hevc_sps(std::uint32_t width, std::uint32_t height,
                                std::uint32_t win_right = 0, std::uint32_t win_bottom = 0) {
    BitWriter w;
    w.u(0, 4);   // sps_video_parameter_set_id
    w.u(0, 3);   // sps_max_sub_layers_minus1 = 0, so no sub-layer PTL entries
    w.flag(true);// sps_temporal_id_nesting_flag
    // profile_tier_level(1, 0): 2+1+5 + 32 + 48 + 8 bits, all inert here.
    w.u(0, 8);
    w.u(0, 32);
    w.u(0, 32);
    w.u(0, 16);
    w.u(120, 8); // general_level_idc (level 4.0)
    w.ue(0);     // sps_seq_parameter_set_id
    w.ue(1);     // chroma_format_idc: 4:2:0, so the window is in 2x2 units
    w.ue(width);
    w.ue(height);
    const bool window = win_right > 0 || win_bottom > 0;
    w.flag(window);
    if (window) {
        w.ue(0);          // left
        w.ue(win_right);
        w.ue(0);          // top
        w.ue(win_bottom);
    }

    std::vector<std::byte> nal;
    nal.push_back(std::byte{0x42}); // nal_unit_type 33 (SPS) << 1
    nal.push_back(std::byte{0x01}); // nuh_layer_id 0, nuh_temporal_id_plus1 1
    const auto rbsp = w.take();
    nal.insert(nal.end(), rbsp.begin(), rbsp.end());
    return nal;
}

} // namespace

TEST(H264SpsTest, DerivesDimensionsFromMacroblockCounts) {
    const auto sps = h264_sps(80, 45); // 1280x720
    const auto parsed = media::h264::parse_sps_dimensions(sps);
    ASSERT_TRUE(parsed.ok()) << std::string(parsed.error().message());
    EXPECT_EQ(parsed.value().width, 1280u);
    EXPECT_EQ(parsed.value().height, 720u);
}

TEST(H264SpsTest, AppliesTheFrameCroppingWindow) {
    // 1080 is not a multiple of 16: a real 1920x1080 SPS codes 68 map units
    // (1088 lines) and crops 4 chroma rows, i.e. 8 luma lines, off the bottom.
    const auto sps = h264_sps(120, 68, /*frame_mbs_only=*/true, /*crop_right=*/0,
                              /*crop_bottom=*/4);
    const auto parsed = media::h264::parse_sps_dimensions(sps);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().width, 1920u);
    EXPECT_EQ(parsed.value().height, 1080u);
}

TEST(H264SpsTest, InterlacedFieldCodingDoublesTheMapUnitHeight) {
    const auto sps = h264_sps(45, 15, /*frame_mbs_only=*/false); // 720x480
    const auto parsed = media::h264::parse_sps_dimensions(sps);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().width, 720u);
    EXPECT_EQ(parsed.value().height, 480u);
}

TEST(H264SpsTest, RejectsANonSpsNalUnit) {
    auto sps = h264_sps(80, 45);
    sps[0] = std::byte{0x68}; // PPS
    EXPECT_FALSE(media::h264::parse_sps_dimensions(sps).ok());
}

TEST(H264SpsTest, RejectsATruncatedSps) {
    const auto sps = h264_sps(80, 45);
    // Keep only the NAL header and one payload byte: every geometry field is
    // gone, so the reader must report an overrun rather than a plausible size.
    const std::vector<std::byte> truncated(sps.begin(), sps.begin() + 2);
    EXPECT_FALSE(media::h264::parse_sps_dimensions(truncated).ok());
}

TEST(H264SpsTest, EmulationPreventionBytesAreRemovedBeforeParsing) {
    // An encoder inserts 0x03 after any 0x0000 pair in the payload. If the
    // parser does not remove it again, every field after that point is read
    // at the wrong bit offset and the geometry comes out wrong -- so escaping
    // a known-good SPS must not change the answer at all.
    const auto sps = h264_sps(80, 45);
    std::vector<std::byte> escaped;
    escaped.push_back(sps[0]); // NAL header is not part of the RBSP
    std::size_t zeros = 0;
    for (std::size_t i = 1; i < sps.size(); ++i) {
        if (zeros >= 2 && static_cast<std::uint8_t>(sps[i]) <= 0x03) {
            escaped.push_back(std::byte{0x03});
            zeros = 0;
        }
        escaped.push_back(sps[i]);
        zeros = (sps[i] == std::byte{0x00}) ? zeros + 1 : 0;
    }

    const auto parsed = media::h264::parse_sps_dimensions(escaped);
    ASSERT_TRUE(parsed.ok()) << std::string(parsed.error().message());
    EXPECT_EQ(parsed.value().width, 1280u);
    EXPECT_EQ(parsed.value().height, 720u);
}

TEST(HevcSpsTest, ReadsLumaSampleDimensionsDirectly) {
    const auto sps = hevc_sps(1280, 720);
    const auto parsed = media::hevc::parse_sps_dimensions(sps);
    ASSERT_TRUE(parsed.ok()) << std::string(parsed.error().message());
    EXPECT_EQ(parsed.value().width, 1280u);
    EXPECT_EQ(parsed.value().height, 720u);
}

TEST(HevcSpsTest, AppliesTheConformanceWindowInChromaUnits) {
    // 4:2:0, so a right offset of 4 removes 8 luma columns.
    const auto sps = hevc_sps(1928, 1080, /*win_right=*/4, /*win_bottom=*/0);
    const auto parsed = media::hevc::parse_sps_dimensions(sps);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().width, 1920u);
    EXPECT_EQ(parsed.value().height, 1080u);
}

TEST(HevcSpsTest, RejectsANonSpsNalUnit) {
    auto sps = hevc_sps(1280, 720);
    sps[0] = std::byte{0x40}; // nal_unit_type 32 (VPS)
    EXPECT_FALSE(media::hevc::parse_sps_dimensions(sps).ok());
}
