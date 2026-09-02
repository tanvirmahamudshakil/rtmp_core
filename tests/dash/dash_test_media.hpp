#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rtmp_server/protocol/chunk/chunk_types.hpp"

// A geometry-valid H.264 SPS/AVCDecoderConfigurationRecord for the DASH
// tests, distinct from tests/hls/test_media.hpp's sps_nal(): the DASH
// segmenter must derive real picture dimensions from the SPS (fMP4 needs
// them for `tkhd`/`avc1`; TS never did), so a placeholder SPS with plausible
// but arbitrary bits is not good enough here — it has to decode to a real
// width/height or the segmenter refuses to build an init segment.
namespace rtmp_server::dash_test {

inline std::byte b(unsigned value) { return static_cast<std::byte>(value & 0xFF); }

inline void append(std::vector<std::byte>& out, std::initializer_list<unsigned> values) {
    for (unsigned v : values) out.push_back(b(v));
}

// Minimal MSB-first bit writer, same shape as tests/media/sps_test.cpp's.
class BitWriter {
public:
    void u(std::uint32_t value, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) put_bit((value >> (count - 1 - i)) & 1u);
    }
    void flag(bool value) { put_bit(value ? 1u : 0u); }
    void ue(std::uint32_t value) {
        const std::uint64_t code = static_cast<std::uint64_t>(value) + 1u;
        std::uint32_t bits = 0;
        while ((code >> bits) > 1u) ++bits;
        u(0, bits);
        u(static_cast<std::uint32_t>(code), bits + 1);
    }
    [[nodiscard]] std::vector<std::byte> take() {
        put_bit(1);
        while (bit_count_ % 8 != 0) put_bit(0);
        return std::move(bytes_);
    }

private:
    void put_bit(std::uint32_t bit) {
        if (bit_count_ % 8 == 0) bytes_.push_back(std::byte{0});
        if (bit != 0) bytes_.back() |= static_cast<std::byte>(1u << (7u - (bit_count_ % 8)));
        ++bit_count_;
    }
    std::vector<std::byte> bytes_;
    std::size_t bit_count_ = 0;
};

// A Baseline-profile SPS for the given picture size, macroblock-aligned (no
// cropping window needed for a synthetic 16x-multiple test resolution).
inline std::vector<std::byte> sps_nal(std::uint32_t width = 320, std::uint32_t height = 240) {
    BitWriter w;
    w.u(66, 8); // profile_idc: Baseline
    w.u(0, 8);  // constraint flags
    w.u(30, 8); // level_idc
    w.ue(0);    // seq_parameter_set_id
    w.ue(4);    // log2_max_frame_num_minus4
    w.ue(0);    // pic_order_cnt_type
    w.ue(4);    // log2_max_pic_order_cnt_lsb_minus4
    w.ue(1);    // max_num_ref_frames
    w.flag(false);
    w.ue(width / 16 - 1);
    w.ue(height / 16 - 1);
    w.flag(true);  // frame_mbs_only_flag
    w.flag(true);  // direct_8x8_inference_flag
    w.flag(false); // frame_cropping_flag
    w.flag(false); // vui_parameters_present_flag

    std::vector<std::byte> nal;
    nal.push_back(std::byte{0x67});
    const auto rbsp = w.take();
    nal.insert(nal.end(), rbsp.begin(), rbsp.end());
    return nal;
}

inline std::vector<std::byte> pps_nal() {
    std::vector<std::byte> nal;
    append(nal, {0x68, 0xCE, 0x3C, 0x80});
    return nal;
}

// AVCDecoderConfigurationRecord wrapped in an FLV video tag body
// (AVCPacketType 0 = sequence header).
inline std::vector<std::byte> avc_sequence_header(std::uint32_t width = 320, std::uint32_t height = 240,
                                                  const std::vector<std::byte>& sps_override = {},
                                                  const std::vector<std::byte>& pps_override = {}) {
    const auto sps = sps_override.empty() ? sps_nal(width, height) : sps_override;
    const auto pps = pps_override.empty() ? pps_nal() : pps_override;

    std::vector<std::byte> out;
    append(out, {0x17, 0x00, 0x00, 0x00, 0x00}); // keyframe|AVC, seq header, cts 0
    append(out, {0x01});
    out.push_back(sps.size() > 3 ? sps[1] : b(0x42));
    out.push_back(sps.size() > 3 ? sps[2] : b(0x00));
    out.push_back(sps.size() > 3 ? sps[3] : b(0x1E));
    append(out, {0xFF, 0xE1});
    append(out, {static_cast<unsigned>((sps.size() >> 8) & 0xFF), static_cast<unsigned>(sps.size() & 0xFF)});
    out.insert(out.end(), sps.begin(), sps.end());
    append(out, {0x01});
    append(out, {static_cast<unsigned>((pps.size() >> 8) & 0xFF), static_cast<unsigned>(pps.size() & 0xFF)});
    out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

// One AVCC video sample as an FLV video tag body.
inline std::vector<std::byte> avc_frame(bool keyframe, std::size_t payload_size = 64,
                                        std::int32_t composition_time = 0) {
    std::vector<std::byte> out;
    out.push_back(b(keyframe ? 0x17 : 0x27));
    append(out, {0x01});
    append(out, {(static_cast<unsigned>(composition_time) >> 16) & 0xFF,
                 (static_cast<unsigned>(composition_time) >> 8) & 0xFF,
                 static_cast<unsigned>(composition_time) & 0xFF});

    const std::size_t nal_size = payload_size + 1;
    append(out, {static_cast<unsigned>((nal_size >> 24) & 0xFF), static_cast<unsigned>((nal_size >> 16) & 0xFF),
                 static_cast<unsigned>((nal_size >> 8) & 0xFF), static_cast<unsigned>(nal_size & 0xFF)});
    out.push_back(b(keyframe ? 0x65 : 0x41));
    for (std::size_t i = 0; i < payload_size; ++i) out.push_back(b(0x88));
    return out;
}

inline std::vector<std::byte> aac_sequence_header(unsigned sample_rate_index = 4, unsigned channels = 2) {
    std::vector<std::byte> out;
    append(out, {0xAF, 0x00});
    const unsigned byte0 = (2u << 3) | ((sample_rate_index >> 1) & 0x07);
    const unsigned byte1 = ((sample_rate_index & 0x01) << 7) | ((channels & 0x0F) << 3);
    append(out, {byte0, byte1});
    return out;
}

inline std::vector<std::byte> aac_frame(std::size_t payload_size = 32) {
    std::vector<std::byte> out;
    append(out, {0xAF, 0x01});
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

} // namespace rtmp_server::dash_test
