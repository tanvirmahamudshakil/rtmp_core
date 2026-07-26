#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rtmp_server/protocol/chunk/chunk_types.hpp"

// Synthetic FLV-form RTMP media payloads for the HLS tests. These are the
// exact byte shapes an RTMP publisher delivers (see docs/hls.md "Input
// format"), built by hand so the tests do not depend on a real encoder.
namespace rtmp_server::hls_test {

inline std::byte b(unsigned value) { return static_cast<std::byte>(value & 0xFF); }

inline void append(std::vector<std::byte>& out, std::initializer_list<unsigned> values) {
    for (unsigned v : values) out.push_back(b(v));
}

// A minimal but structurally valid SPS (Baseline profile 66, level 3.0) and
// PPS. The bit contents beyond the first four bytes are not decoded by the
// packager (it is passthrough), but the NAL headers and lengths are.
inline std::vector<std::byte> sps_nal() {
    std::vector<std::byte> nal;
    // 0x67 = NAL header (nal_ref_idc 3, type 7 = SPS)
    append(nal, {0x67, 0x42, 0xC0, 0x1E, 0xD9, 0x00, 0xF0, 0x11, 0x7E, 0xF0, 0x10, 0x10});
    return nal;
}

inline std::vector<std::byte> pps_nal() {
    std::vector<std::byte> nal;
    append(nal, {0x68, 0xCE, 0x3C, 0x80}); // type 8 = PPS
    return nal;
}

// AVCDecoderConfigurationRecord wrapped in an FLV video tag body
// (AVCPacketType 0 = sequence header).
inline std::vector<std::byte> avc_sequence_header(const std::vector<std::byte>& sps = sps_nal(),
                                                   const std::vector<std::byte>& pps = pps_nal()) {
    std::vector<std::byte> out;
    append(out, {0x17, 0x00, 0x00, 0x00, 0x00}); // keyframe|AVC, seq header, cts 0

    append(out, {0x01});                       // configurationVersion
    out.push_back(sps.size() > 3 ? sps[1] : b(0x42)); // AVCProfileIndication
    out.push_back(sps.size() > 3 ? sps[2] : b(0xC0)); // profile_compatibility
    out.push_back(sps.size() > 3 ? sps[3] : b(0x1E)); // AVCLevelIndication
    append(out, {0xFF});                       // 6 reserved bits + lengthSizeMinusOne = 3 (4 bytes)
    append(out, {0xE1});                       // 3 reserved bits + numOfSPS = 1
    append(out, {static_cast<unsigned>((sps.size() >> 8) & 0xFF), static_cast<unsigned>(sps.size() & 0xFF)});
    out.insert(out.end(), sps.begin(), sps.end());
    append(out, {0x01});                       // numOfPPS = 1
    append(out, {static_cast<unsigned>((pps.size() >> 8) & 0xFF), static_cast<unsigned>(pps.size() & 0xFF)});
    out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

// One AVCC video sample as an FLV video tag body. `payload_size` controls the
// synthetic slice length so tests can drive segment size bounds.
inline std::vector<std::byte> avc_frame(bool keyframe, std::size_t payload_size = 64,
                                        std::int32_t composition_time = 0) {
    std::vector<std::byte> out;
    out.push_back(b(keyframe ? 0x17 : 0x27)); // FrameType 1/2, CodecID 7
    append(out, {0x01});                      // AVCPacketType 1 = NALU
    append(out, {(static_cast<unsigned>(composition_time) >> 16) & 0xFF,
                 (static_cast<unsigned>(composition_time) >> 8) & 0xFF,
                 static_cast<unsigned>(composition_time) & 0xFF});

    // One length-prefixed NAL: IDR (type 5) for keyframes, non-IDR (1) else.
    const std::size_t nal_size = payload_size + 1;
    append(out, {static_cast<unsigned>((nal_size >> 24) & 0xFF), static_cast<unsigned>((nal_size >> 16) & 0xFF),
                 static_cast<unsigned>((nal_size >> 8) & 0xFF), static_cast<unsigned>(nal_size & 0xFF)});
    out.push_back(b(keyframe ? 0x65 : 0x41));
    for (std::size_t i = 0; i < payload_size; ++i) out.push_back(b(0x88));
    return out;
}

// AudioSpecificConfig (AAC-LC, 44100 Hz, stereo) in an FLV audio tag body.
inline std::vector<std::byte> aac_sequence_header(unsigned sample_rate_index = 4,
                                                   unsigned channels = 2) {
    std::vector<std::byte> out;
    append(out, {0xAF, 0x00}); // AAC, sequence header
    // audioObjectType=2 (5 bits), samplingFrequencyIndex (4), channelConfig (4)
    const unsigned byte0 = (2u << 3) | ((sample_rate_index >> 1) & 0x07);
    const unsigned byte1 = ((sample_rate_index & 0x01) << 7) | ((channels & 0x0F) << 3);
    append(out, {byte0, byte1});
    return out;
}

inline std::vector<std::byte> aac_frame(std::size_t payload_size = 32) {
    std::vector<std::byte> out;
    append(out, {0xAF, 0x01}); // AAC, raw frame
    for (std::size_t i = 0; i < payload_size; ++i) out.push_back(b(0x21));
    return out;
}

inline protocol::chunk::RtmpMessage message(std::uint8_t type_id, std::uint32_t timestamp,
                                            std::vector<std::byte> payload) {
    protocol::chunk::RtmpMessage m;
    m.message_type_id = type_id;
    m.timestamp = timestamp;
    m.payload = std::move(payload);
    return m;
}

inline protocol::chunk::RtmpMessage video_message(std::uint32_t timestamp, std::vector<std::byte> payload) {
    return message(9, timestamp, std::move(payload));
}

inline protocol::chunk::RtmpMessage audio_message(std::uint32_t timestamp, std::vector<std::byte> payload) {
    return message(8, timestamp, std::move(payload));
}

} // namespace rtmp_server::hls_test
