#include "rtmp_server/relay/rtmp_tag_builder.hpp"

#include <algorithm>

#include "rtmp_server/media/aac/adts.hpp"

namespace rtmp_server::relay {
namespace {

core::Error malformed(std::string message) {
    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                       std::move(message));
}

core::Error out_of_order(std::string message) {
    return core::Error(core::ErrorCode::InvalidStateTransition, core::ErrorCategory::Protocol,
                       std::move(message));
}

void append_be24(std::vector<std::byte>& out, std::int32_t value) {
    out.push_back(static_cast<std::byte>((value >> 16) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(value & 0xff));
}

void append_be32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xff));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    out.push_back(static_cast<std::byte>(value & 0xff));
}

std::uint8_t nal_type_of(std::span<const std::byte> nal) {
    return nal.empty() ? 0 : static_cast<std::uint8_t>(nal[0]) & 0x1F;
}

} // namespace

std::vector<std::span<const std::byte>> split_annexb_nal_units(std::span<const std::byte> annexb) {
    std::vector<std::span<const std::byte>> units;
    std::size_t i = 0;
    // Finds the next start code (3- or 4-byte 00 00 01 / 00 00 00 01) at or
    // after `from`, returning {position, code_length}, or {size, 0} if none.
    const auto next_start_code = [&](std::size_t from) -> std::pair<std::size_t, std::size_t> {
        for (std::size_t i2 = from; i2 + 2 < annexb.size(); ++i2) {
            if (annexb[i2] == std::byte{0} && annexb[i2 + 1] == std::byte{0} &&
                annexb[i2 + 2] == std::byte{1}) {
                if (i2 > from && annexb[i2 - 1] == std::byte{0}) return {i2 - 1, 4};
                return {i2, 3};
            }
        }
        return {annexb.size(), 0};
    };

    auto [start, code_length] = next_start_code(0);
    if (code_length == 0) return units; // no start code at all: not Annex B
    i = start + code_length;
    while (i < annexb.size()) {
        auto [next, next_length] = next_start_code(i);
        if (next > i) units.push_back(annexb.subspan(i, next - i));
        if (next_length == 0) break;
        i = next + next_length;
    }
    return units;
}

core::Result<std::vector<std::byte>> RtmpVideoTagBuilder::build_sequence_header(
    std::span<const std::byte> keyframe_annexb) {
    std::span<const std::byte> sps;
    std::span<const std::byte> pps;
    for (const auto& nal : split_annexb_nal_units(keyframe_annexb)) {
        const auto type = nal_type_of(nal);
        if (type == media::h264::kNalTypeSps && sps.empty()) sps = nal;
        if (type == media::h264::kNalTypePps && pps.empty()) pps = nal;
    }
    if (sps.empty() || pps.empty()) {
        return malformed("encoder keyframe carried no SPS/PPS to build a sequence header from");
    }
    if (sps.size() < 4) return malformed("SPS too short to read profile/level");

    config_ = media::h264::AvcDecoderConfig{};
    config_.nalu_length_size = 4;
    config_.sps.push_back(std::vector<std::byte>(sps.begin(), sps.end()));
    config_.pps.push_back(std::vector<std::byte>(pps.begin(), pps.end()));

    std::vector<std::byte> tag;
    tag.push_back(std::byte{0x17}); // keyframe, AVC
    tag.push_back(std::byte{0x00}); // AVCPacketType: sequence header
    append_be24(tag, 0);            // composition time: n/a for a sequence header

    tag.push_back(std::byte{0x01});  // configurationVersion
    tag.push_back(sps[1]);           // AVCProfileIndication
    tag.push_back(sps[2]);           // profile_compatibility
    tag.push_back(sps[3]);           // AVCLevelIndication
    tag.push_back(std::byte{0xFF});  // reserved(6)=1 | lengthSizeMinusOne(2)=3 (4-byte lengths)

    tag.push_back(std::byte{0xE1}); // reserved(3)=1 | numOfSequenceParameterSets(5)=1
    tag.push_back(static_cast<std::byte>((sps.size() >> 8) & 0xff));
    tag.push_back(static_cast<std::byte>(sps.size() & 0xff));
    tag.insert(tag.end(), sps.begin(), sps.end());

    tag.push_back(std::byte{0x01}); // numOfPictureParameterSets = 1
    tag.push_back(static_cast<std::byte>((pps.size() >> 8) & 0xff));
    tag.push_back(static_cast<std::byte>(pps.size() & 0xff));
    tag.insert(tag.end(), pps.begin(), pps.end());

    return tag;
}

core::Result<std::vector<std::byte>> RtmpVideoTagBuilder::build_frame(
    std::span<const std::byte> annexb, std::int64_t pts_90k, std::int64_t dts_90k, bool keyframe) {
    if (!config_.valid()) {
        return out_of_order("build_frame called before a sequence header was built and published");
    }

    std::vector<std::byte> tag;
    tag.push_back(keyframe ? std::byte{0x17} : std::byte{0x27});
    tag.push_back(std::byte{0x01}); // AVCPacketType: NALU
    // Composition time is (PTS - DTS) in the RTMP tag's millisecond units; the
    // caller's clock is 90 kHz, matching RtmpTagConverter's own scale on the
    // decode side.
    const auto composition_time_ms = static_cast<std::int32_t>((pts_90k - dts_90k) / 90);
    append_be24(tag, composition_time_ms);

    // Parameter sets are carried once, in the sequence header, not repeated
    // per keyframe here -- a target that wants mid-stream parameter-set
    // refresh support would need this rebuilt to detect an SPS/PPS change and
    // republish the sequence header, which this builder does not attempt
    // (matching the native encoders, which do not change parameter sets
    // mid-stream either).
    for (const auto& nal : split_annexb_nal_units(annexb)) {
        const auto type = nal_type_of(nal);
        if (type == media::h264::kNalTypeSps || type == media::h264::kNalTypePps) continue;
        append_be32(tag, static_cast<std::uint32_t>(nal.size()));
        tag.insert(tag.end(), nal.begin(), nal.end());
    }
    return tag;
}

core::Result<std::vector<std::byte>> RtmpAudioTagBuilder::build_sequence_header(
    std::span<const std::byte> adts_frame) {
    if (adts_frame.size() < media::aac::kAdtsHeaderSize) {
        return malformed("ADTS frame too short to carry a header");
    }
    if (adts_frame[0] != std::byte{0xFF} ||
        (static_cast<std::uint8_t>(adts_frame[1]) & 0xF0) != 0xF0) {
        return malformed("ADTS syncword not found");
    }
    // AAC LC only: ADTS's 2-bit "profile" field cannot itself distinguish
    // plain AAC-LC from implicit-signalled HE-AAC/HE-AACv2 (SBR/PS ride in an
    // ADTS extension this builder does not parse), so a source encoder using
    // SBR would be described here as plain AAC-LC. Every native AAC rendition
    // this pipeline produces is built by AacEncoder from a fixed, known
    // object type per rendition; a future caller that needs SBR-aware
    // dispatch would pass the real object type in rather than deriving it
    // from the ADTS byte.
    const auto profile = (static_cast<std::uint8_t>(adts_frame[2]) >> 6) & 0x3;
    const auto sampling_frequency_index = (static_cast<std::uint8_t>(adts_frame[2]) >> 2) & 0xF;
    const auto channel_configuration =
        static_cast<std::uint8_t>(((static_cast<std::uint8_t>(adts_frame[2]) & 0x1) << 2) |
                                  ((static_cast<std::uint8_t>(adts_frame[3]) >> 6) & 0x3));
    const std::uint8_t object_type = static_cast<std::uint8_t>(profile + 1);

    std::vector<std::byte> tag;
    tag.push_back(std::byte{0xAF}); // SoundFormat=AAC(10), rate/size/type bits (ignored for AAC)
    tag.push_back(std::byte{0x00}); // AACPacketType: sequence header

    // 2-byte AudioSpecificConfig: audioObjectType(5) samplingFrequencyIndex(4)
    // channelConfiguration(4) frameLengthFlag/dependsOnCoreCoder/
    // extensionFlag(3, all zero -- no GASpecificConfig extension).
    const std::uint16_t asc =
        static_cast<std::uint16_t>((object_type & 0x1F) << 11) |
        static_cast<std::uint16_t>((sampling_frequency_index & 0xF) << 7) |
        static_cast<std::uint16_t>((channel_configuration & 0xF) << 3);
    tag.push_back(static_cast<std::byte>((asc >> 8) & 0xff));
    tag.push_back(static_cast<std::byte>(asc & 0xff));

    sequence_header_sent_ = true;
    return tag;
}

core::Result<std::vector<std::byte>> RtmpAudioTagBuilder::build_frame(
    std::span<const std::byte> adts_frame) {
    if (!sequence_header_sent_) {
        return out_of_order("build_frame called before a sequence header was built and published");
    }
    if (adts_frame.size() < media::aac::kAdtsHeaderSize) {
        return malformed("ADTS frame too short to carry a header");
    }
    std::vector<std::byte> tag;
    tag.push_back(std::byte{0xAF});
    tag.push_back(std::byte{0x01}); // AACPacketType: raw
    tag.insert(tag.end(), adts_frame.begin() + static_cast<std::ptrdiff_t>(media::aac::kAdtsHeaderSize),
              adts_frame.end());
    return tag;
}

} // namespace rtmp_server::relay
