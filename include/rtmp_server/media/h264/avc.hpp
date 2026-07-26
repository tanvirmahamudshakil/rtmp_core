#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"

// H.264 bitstream helpers needed to repackage RTMP/FLV video payloads into
// MPEG-TS without re-encoding (passthrough only — docs/v2_promot.md Phase 6
// "Do not build a raw H.264/AAC encoder from scratch").
//
// RTMP delivers video as FLV VIDEODATA: a 1-byte FrameType/CodecID, a 1-byte
// AVCPacketType, a 3-byte composition-time offset, then either an
// AVCDecoderConfigurationRecord (AVCPacketType 0, "sequence header") or a
// length-prefixed AVCC sample (AVCPacketType 1). MPEG-TS carries Annex B
// (start-code prefixed) NAL units instead, so this layer converts between
// them. Nothing here allocates based on an unvalidated client length.
namespace rtmp_server::media::h264 {

inline constexpr std::uint8_t kFrameTypeKey = 1; // FLV FrameType nibble for a keyframe
inline constexpr std::uint8_t kCodecIdAvc = 7;   // FLV CodecID nibble for AVC

inline constexpr std::uint8_t kAvcPacketTypeSequenceHeader = 0;
inline constexpr std::uint8_t kAvcPacketTypeNalu = 1;
inline constexpr std::uint8_t kAvcPacketTypeEndOfSequence = 2;

// NAL unit types we must recognise for correct TS packaging.
inline constexpr std::uint8_t kNalTypeIdr = 5;
inline constexpr std::uint8_t kNalTypeSei = 6;
inline constexpr std::uint8_t kNalTypeSps = 7;
inline constexpr std::uint8_t kNalTypePps = 8;
inline constexpr std::uint8_t kNalTypeAud = 9;

// Parsed AVCDecoderConfigurationRecord: the SPS/PPS an MPEG-TS keyframe must
// be prefixed with, plus the NAL length field width used by AVCC samples.
struct AvcDecoderConfig {
    std::uint8_t nalu_length_size = 4; // 1..4
    std::vector<std::vector<std::byte>> sps;
    std::vector<std::vector<std::byte>> pps;

    [[nodiscard]] bool valid() const noexcept { return !sps.empty() && !pps.empty(); }
};

// The parsed shape of one FLV video payload.
struct VideoTag {
    bool is_keyframe = false;
    std::uint8_t avc_packet_type = kAvcPacketTypeNalu;
    std::int32_t composition_time_ms = 0; // signed 24-bit CTS offset (PTS - DTS)
    std::span<const std::byte> body;      // config record or AVCC sample data
};

// Parses the 5-byte FLV video header. Fails (MalformedChunk/Protocol) on a
// truncated payload or a non-AVC codec id.
[[nodiscard]] core::Result<VideoTag> parse_video_tag(std::span<const std::byte> payload);

// Parses an AVCDecoderConfigurationRecord. Every length field is validated
// against the remaining buffer before use.
[[nodiscard]] core::Result<AvcDecoderConfig> parse_decoder_config(std::span<const std::byte> record);

// Converts a length-prefixed AVCC sample to Annex B, appending
// 4-byte start codes to `out`. When `insert_parameter_sets` is true the
// config's SPS/PPS are emitted first (required on every TS keyframe so a
// player can join mid-stream). Returns an error if a NAL length field runs
// past the end of the sample.
[[nodiscard]] core::Result<void> avcc_to_annexb(std::span<const std::byte> sample,
                                                const AvcDecoderConfig& config, bool insert_parameter_sets,
                                                std::vector<std::byte>& out);

} // namespace rtmp_server::media::h264
