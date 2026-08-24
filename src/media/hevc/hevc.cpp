#include "rtmp_server/media/hevc/hevc.hpp"

namespace rtmp_server::media::hevc {

namespace {

core::Error malformed(std::string_view what) {
    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol, what);
}

std::uint8_t byte_at(std::span<const std::byte> data, std::size_t i) {
    return static_cast<std::uint8_t>(data[i]);
}

std::uint32_t read_be(std::span<const std::byte> data, std::size_t offset, std::uint8_t width) {
    std::uint32_t value = 0;
    for (std::uint8_t i = 0; i < width; ++i) {
        value = (value << 8) | byte_at(data, offset + i);
    }
    return value;
}

void append_start_code(std::vector<std::byte>& out) {
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x01});
}

} // namespace

core::Result<HevcDecoderConfig> parse_decoder_config(std::span<const std::byte> record) {
    // HEVCDecoderConfigurationRecord (ISO 14496-15 8.3.3.1), fixed header
    // before the NAL array list:
    //   configurationVersion(1)
    //   general_profile_space(2)+general_tier_flag(1)+general_profile_idc(5) (1)
    //   general_profile_compatibility_flags(4)
    //   general_constraint_indicator_flags(6)
    //   general_level_idc(1)
    //   reserved(4)+min_spatial_segmentation_idc(12) (2)
    //   reserved(6)+parallelismType(2) (1)
    //   reserved(6)+chroma_format_idc(2) (1)
    //   reserved(5)+bit_depth_luma_minus8(3) (1)
    //   reserved(5)+bit_depth_chroma_minus8(3) (1)
    //   avgFrameRate(2)
    //   constantFrameRate(2)+numTemporalLayers(3)+temporalIdNested(1)+lengthSizeMinusOne(2) (1)
    //   numOfArrays(1)
    // = 23 bytes, followed by numOfArrays NAL-array entries.
    constexpr std::size_t kFixedHeaderSize = 23;
    if (record.size() < kFixedHeaderSize) {
        return malformed("HEVCDecoderConfigurationRecord shorter than the fixed 23-byte header");
    }

    HevcDecoderConfig config;
    config.nalu_length_size = static_cast<std::uint8_t>((byte_at(record, 21) & 0x03) + 1);

    const std::size_t num_arrays = byte_at(record, 22);
    std::size_t offset = kFixedHeaderSize;

    for (std::size_t a = 0; a < num_arrays; ++a) {
        if (offset + 3 > record.size()) return malformed("truncated HEVC NAL array header");
        const std::uint8_t nal_unit_type = byte_at(record, offset) & 0x3F;
        offset += 1;
        const std::size_t num_nalus = read_be(record, offset, 2);
        offset += 2;

        for (std::size_t i = 0; i < num_nalus; ++i) {
            if (offset + 2 > record.size()) return malformed("truncated HEVC NAL length field");
            const std::size_t len = read_be(record, offset, 2);
            offset += 2;
            if (offset + len > record.size()) return malformed("HEVC NAL runs past end of record");
            std::vector<std::byte> nal(record.begin() + static_cast<std::ptrdiff_t>(offset),
                                       record.begin() + static_cast<std::ptrdiff_t>(offset + len));
            if (nal_unit_type == kNalTypeVps) {
                config.vps.push_back(std::move(nal));
            } else if (nal_unit_type == kNalTypeSps) {
                config.sps.push_back(std::move(nal));
            } else if (nal_unit_type == kNalTypePps) {
                config.pps.push_back(std::move(nal));
            }
            // Any other array type (e.g. SEI/prefix) is intentionally
            // skipped: not needed to build a decodable/joinable Annex B
            // access unit.
            offset += len;
        }
    }

    if (!config.valid()) return malformed("HEVC config carries no SPS/PPS");
    return config;
}

core::Result<void> hvcc_to_annexb(std::span<const std::byte> sample, const HevcDecoderConfig& config,
                                  bool insert_parameter_sets, std::vector<std::byte>& out) {
    if (config.nalu_length_size < 1 || config.nalu_length_size > 4) {
        return malformed("invalid HEVC NAL length size");
    }

    if (insert_parameter_sets) {
        for (const auto& vps : config.vps) {
            append_start_code(out);
            out.insert(out.end(), vps.begin(), vps.end());
        }
        for (const auto& sps : config.sps) {
            append_start_code(out);
            out.insert(out.end(), sps.begin(), sps.end());
        }
        for (const auto& pps : config.pps) {
            append_start_code(out);
            out.insert(out.end(), pps.begin(), pps.end());
        }
    }

    std::size_t offset = 0;
    while (offset < sample.size()) {
        if (offset + config.nalu_length_size > sample.size()) {
            return malformed("truncated HVCC NAL length prefix");
        }
        const std::uint32_t length = read_be(sample, offset, config.nalu_length_size);
        offset += config.nalu_length_size;
        if (length == 0) continue;
        if (offset + length > sample.size()) return malformed("HVCC NAL runs past end of sample");

        // HEVC NAL header: forbidden_zero_bit(1)+nal_unit_type(6)+layer_id(1
        // of 6, MSB)... type occupies bits 1-6 of the first header byte.
        const std::uint8_t nal_type = (byte_at(sample, offset) >> 1) & 0x3F;
        const bool skip = (nal_type == kNalTypeAud) ||
                          (insert_parameter_sets &&
                           (nal_type == kNalTypeVps || nal_type == kNalTypeSps || nal_type == kNalTypePps));
        if (!skip) {
            append_start_code(out);
            out.insert(out.end(), sample.begin() + static_cast<std::ptrdiff_t>(offset),
                       sample.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        offset += length;
    }

    return {};
}

} // namespace rtmp_server::media::hevc
