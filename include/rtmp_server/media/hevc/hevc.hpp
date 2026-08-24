#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"

// HEVC bitstream helpers, mirroring media::h264::avc.hpp (docs there explain
// the overall AVCC/HVCC -> Annex B passthrough shape). Needed for two
// distinct callers: the Enhanced RTMP (`hvc1` FourCC) ingest path
// (protocol::media::MediaIngest, see media_ingest.hpp's HevcSequenceHeader)
// and the native source-transcode pipeline pulling an HEVC RTMP source
// (transcoding::native::RtmpSourceClient via hls_source_puller.cpp), both of
// which need to turn a length-prefixed HVCC sample into Annex B before it can
// reach TsMuxer or HevcDecoder.
namespace rtmp_server::media::hevc {

// HEVC NAL unit types (ITU-T H.265 Table 7-1) this layer must recognise.
inline constexpr std::uint8_t kNalTypeVps = 32;
inline constexpr std::uint8_t kNalTypeSps = 33;
inline constexpr std::uint8_t kNalTypePps = 34;
inline constexpr std::uint8_t kNalTypeAud = 35;
inline constexpr std::uint8_t kNalTypeIdrWRadl = 19;
inline constexpr std::uint8_t kNalTypeIdrNLp = 20;
inline constexpr std::uint8_t kNalTypeCra = 21;

// Parsed HEVCDecoderConfigurationRecord (ISO 14496-15 8.3.3.1). Only the
// fields a passthrough/decode consumer needs are kept: the profile/level
// fixed-header fields are skipped over but not retained (nothing downstream
// currently needs them -- add fields here if that changes).
struct HevcDecoderConfig {
    std::uint8_t nalu_length_size = 4; // lengthSizeMinusOne + 1
    std::vector<std::vector<std::byte>> vps;
    std::vector<std::vector<std::byte>> sps;
    std::vector<std::vector<std::byte>> pps;

    [[nodiscard]] bool valid() const noexcept { return !sps.empty() && !pps.empty(); }
};

// Parses an HEVCDecoderConfigurationRecord. Every length field is validated
// against the remaining buffer before use. Only VPS/SPS/PPS NAL arrays are
// retained; any other array type (e.g. SEI) is skipped.
[[nodiscard]] core::Result<HevcDecoderConfig> parse_decoder_config(std::span<const std::byte> record);

// Converts a length-prefixed HVCC sample to Annex B, appending 4-byte start
// codes to `out`. When `insert_parameter_sets` is true the config's
// VPS/SPS/PPS are emitted first (required on every keyframe so a decoder or
// TS demuxer can join mid-stream), mirroring media::h264::avcc_to_annexb.
[[nodiscard]] core::Result<void> hvcc_to_annexb(std::span<const std::byte> sample,
                                                const HevcDecoderConfig& config,
                                                bool insert_parameter_sets, std::vector<std::byte>& out);

} // namespace rtmp_server::media::hevc
