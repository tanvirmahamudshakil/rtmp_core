#include "rtmp_server/media/h264/avc.hpp"

namespace rtmp_server::media::h264 {

namespace {

core::Error malformed(std::string_view what) {
    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol, what);
}

std::uint8_t byte_at(std::span<const std::byte> data, std::size_t i) {
    return static_cast<std::uint8_t>(data[i]);
}

// Reads a big-endian unsigned integer of `width` (1..4) bytes.
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

core::Result<VideoTag> parse_video_tag(std::span<const std::byte> payload) {
    // FrameType(4b)|CodecID(4b), AVCPacketType(1), CompositionTime(3)
    if (payload.size() < 5) return malformed("FLV video payload shorter than 5 bytes");

    const std::uint8_t header = byte_at(payload, 0);
    const std::uint8_t frame_type = (header >> 4) & 0x0F;
    const std::uint8_t codec_id = header & 0x0F;
    if (codec_id != kCodecIdAvc) {
        return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                           "unsupported FLV video codec id (HLS passthrough requires AVC/H.264)");
    }

    VideoTag tag;
    tag.is_keyframe = (frame_type == kFrameTypeKey);
    tag.avc_packet_type = byte_at(payload, 1);

    // CompositionTime is a signed 24-bit value; sign-extend it.
    std::int32_t cts = static_cast<std::int32_t>(read_be(payload, 2, 3));
    if ((cts & 0x00800000) != 0) cts |= static_cast<std::int32_t>(0xFF000000u);
    tag.composition_time_ms = cts;

    tag.body = payload.subspan(5);
    return tag;
}

core::Result<AvcDecoderConfig> parse_decoder_config(std::span<const std::byte> record) {
    // configurationVersion(1) profile(1) compat(1) level(1)
    // reserved(6b)+lengthSizeMinusOne(2b) reserved(3b)+numSPS(5b)
    if (record.size() < 6) return malformed("AVCDecoderConfigurationRecord too short");

    AvcDecoderConfig config;
    config.nalu_length_size = static_cast<std::uint8_t>((byte_at(record, 4) & 0x03) + 1);

    std::size_t offset = 5;
    const std::size_t sps_count = byte_at(record, offset) & 0x1F;
    offset += 1;

    for (std::size_t i = 0; i < sps_count; ++i) {
        if (offset + 2 > record.size()) return malformed("truncated SPS length");
        const std::size_t len = read_be(record, offset, 2);
        offset += 2;
        if (offset + len > record.size()) return malformed("SPS runs past end of record");
        config.sps.emplace_back(record.begin() + static_cast<std::ptrdiff_t>(offset),
                                record.begin() + static_cast<std::ptrdiff_t>(offset + len));
        offset += len;
    }

    if (offset >= record.size()) return malformed("missing PPS count");
    const std::size_t pps_count = byte_at(record, offset);
    offset += 1;

    for (std::size_t i = 0; i < pps_count; ++i) {
        if (offset + 2 > record.size()) return malformed("truncated PPS length");
        const std::size_t len = read_be(record, offset, 2);
        offset += 2;
        if (offset + len > record.size()) return malformed("PPS runs past end of record");
        config.pps.emplace_back(record.begin() + static_cast<std::ptrdiff_t>(offset),
                                record.begin() + static_cast<std::ptrdiff_t>(offset + len));
        offset += len;
    }

    if (!config.valid()) return malformed("AVC config carries no SPS/PPS");
    return config;
}

core::Result<void> avcc_to_annexb(std::span<const std::byte> sample, const AvcDecoderConfig& config,
                                  bool insert_parameter_sets, std::vector<std::byte>& out) {
    if (config.nalu_length_size < 1 || config.nalu_length_size > 4) {
        return malformed("invalid NAL length size");
    }

    // An Access Unit Delimiter makes the elementary stream well-formed for
    // strict TS demuxers and marks the AU boundary unambiguously.
    append_start_code(out);
    out.push_back(std::byte{0x09});
    out.push_back(std::byte{0xF0});

    if (insert_parameter_sets) {
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
            return malformed("truncated AVCC NAL length prefix");
        }
        const std::uint32_t length = read_be(sample, offset, config.nalu_length_size);
        offset += config.nalu_length_size;
        if (length == 0) continue;
        if (offset + length > sample.size()) return malformed("AVCC NAL runs past end of sample");

        const std::uint8_t nal_type = byte_at(sample, offset) & 0x1F;
        // Drop NALs the muxer supplies itself: a second AUD would be
        // illegal, and in-band SPS/PPS are already emitted above on
        // keyframes (emitting them twice confuses some demuxers).
        const bool skip = (nal_type == kNalTypeAud) ||
                          (insert_parameter_sets && (nal_type == kNalTypeSps || nal_type == kNalTypePps));
        if (!skip) {
            append_start_code(out);
            out.insert(out.end(), sample.begin() + static_cast<std::ptrdiff_t>(offset),
                       sample.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        offset += length;
    }

    return {};
}

} // namespace rtmp_server::media::h264
