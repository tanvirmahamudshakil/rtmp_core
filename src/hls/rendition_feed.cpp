#include "rtmp_server/hls/rendition_feed.hpp"

#include <array>
#include <vector>

#include "rtmp_server/media/h264/avc.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"

namespace rtmp_server::hls {

namespace {

using protocol::chunk::RtmpMessage;

std::uint8_t u8(std::span<const std::byte> b, std::size_t i) {
    return static_cast<std::uint8_t>(b[i]);
}
void put_u8(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
}
void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    put_u8(out, v >> 24);
    put_u8(out, v >> 16);
    put_u8(out, v >> 8);
    put_u8(out, v);
}

std::int64_t to_ms(std::int64_t ts_90k) { return ts_90k / 90; }

// Splits an Annex B buffer into individual NAL units (payload without the start
// code). Handles both 3- and 4-byte start codes.
std::vector<std::span<const std::byte>> split_nals(std::span<const std::byte> annexb) {
    std::vector<std::span<const std::byte>> nals;
    const std::size_t n = annexb.size();
    std::size_t i = 0;
    // Find first start code.
    auto is_sc = [&](std::size_t p, std::size_t& sc_len) {
        if (p + 3 <= n && u8(annexb, p) == 0 && u8(annexb, p + 1) == 0 && u8(annexb, p + 2) == 1) {
            sc_len = 3;
            return true;
        }
        if (p + 4 <= n && u8(annexb, p) == 0 && u8(annexb, p + 1) == 0 && u8(annexb, p + 2) == 0 &&
            u8(annexb, p + 3) == 1) {
            sc_len = 4;
            return true;
        }
        return false;
    };
    std::size_t sc_len = 0;
    while (i < n && !is_sc(i, sc_len)) ++i;
    while (i < n) {
        i += sc_len;
        const std::size_t start = i;
        std::size_t next = i;
        std::size_t next_sc = 0;
        while (next < n && !is_sc(next, next_sc)) ++next;
        if (next > start) nals.push_back(annexb.subspan(start, next - start));
        i = next;
        sc_len = next_sc;
    }
    return nals;
}

RtmpMessage make_message(std::uint8_t type_id, std::int64_t timestamp_ms, std::vector<std::byte> body) {
    RtmpMessage m;
    m.message_type_id = type_id;
    m.timestamp = static_cast<std::uint32_t>(timestamp_ms < 0 ? 0 : timestamp_ms);
    m.payload = std::move(body);
    return m;
}

} // namespace

void RenditionFeed::push_video(std::span<const std::byte> annexb, std::int64_t pts_90k,
                               std::int64_t dts_90k, bool keyframe) {
    if (annexb.empty()) return;
    const auto nals = split_nals(annexb);
    if (nals.empty()) return;

    std::span<const std::byte> sps;
    std::span<const std::byte> pps;
    for (const auto& nal : nals) {
        if (nal.empty()) continue;
        const std::uint8_t type = u8(nal, 0) & 0x1F;
        if (type == media::h264::kNalTypeSps && sps.empty()) sps = nal;
        else if (type == media::h264::kNalTypePps && pps.empty()) pps = nal;
    }

    const std::int64_t dts_ms = to_ms(dts_90k);

    // Emit the AVCDecoderConfigurationRecord once, from the first keyframe.
    if (!video_config_sent_ && !sps.empty() && !pps.empty() && sps.size() >= 4) {
        std::vector<std::byte> body;
        put_u8(body, 0x17); // keyframe | AVC
        put_u8(body, 0x00); // AVCPacketType 0 = sequence header
        put_u8(body, 0x00);
        put_u8(body, 0x00);
        put_u8(body, 0x00); // composition time 0
        put_u8(body, 0x01); // configurationVersion
        put_u8(body, u8(sps, 1)); // profile
        put_u8(body, u8(sps, 2)); // compatibility
        put_u8(body, u8(sps, 3)); // level
        put_u8(body, 0xFF);       // lengthSizeMinusOne = 3
        put_u8(body, 0xE1);       // numOfSPS = 1
        put_u8(body, static_cast<std::uint32_t>(sps.size() >> 8));
        put_u8(body, static_cast<std::uint32_t>(sps.size() & 0xFF));
        body.insert(body.end(), sps.begin(), sps.end());
        put_u8(body, 0x01); // numOfPPS = 1
        put_u8(body, static_cast<std::uint32_t>(pps.size() >> 8));
        put_u8(body, static_cast<std::uint32_t>(pps.size() & 0xFF));
        body.insert(body.end(), pps.begin(), pps.end());
        segmenter_.on_video(make_message(9, dts_ms, std::move(body)));
        video_config_sent_ = true;
    }
    if (!video_config_sent_) return; // wait for a keyframe with parameter sets

    // Build the AVCC sample: length-prefixed slice NALs (parameter sets live in
    // the config record; the segmenter re-inserts them on each keyframe).
    std::vector<std::byte> body;
    put_u8(body, keyframe ? 0x17 : 0x27);
    put_u8(body, 0x01); // AVCPacketType 1 = NALU
    const std::int64_t cts = to_ms(pts_90k - dts_90k);
    const std::uint32_t cts24 = static_cast<std::uint32_t>(cts) & 0xFFFFFF;
    put_u8(body, cts24 >> 16);
    put_u8(body, cts24 >> 8);
    put_u8(body, cts24);
    bool wrote_slice = false;
    for (const auto& nal : nals) {
        if (nal.empty()) continue;
        const std::uint8_t type = u8(nal, 0) & 0x1F;
        if (type == media::h264::kNalTypeSps || type == media::h264::kNalTypePps ||
            type == media::h264::kNalTypeAud) {
            continue; // not carried in the AVCC sample
        }
        put_u32(body, static_cast<std::uint32_t>(nal.size()));
        body.insert(body.end(), nal.begin(), nal.end());
        wrote_slice = true;
    }
    if (wrote_slice) segmenter_.on_video(make_message(9, dts_ms, std::move(body)));
}

void RenditionFeed::push_audio(std::span<const std::byte> adts, std::int64_t pts_90k) {
    if (adts.size() < 7) return;

    // ADTS fixed header fields needed to derive the AudioSpecificConfig.
    const std::uint8_t profile = (u8(adts, 2) >> 6) & 0x03; // object_type - 1
    const std::uint8_t freq_index = (u8(adts, 2) >> 2) & 0x0F;
    const std::uint8_t channel_cfg =
        static_cast<std::uint8_t>(((u8(adts, 2) & 0x01) << 2) | ((u8(adts, 3) >> 6) & 0x03));
    const bool protection_absent = (u8(adts, 1) & 0x01) != 0;
    const std::size_t header_len = protection_absent ? 7u : 9u;
    if (adts.size() <= header_len) return;

    const std::int64_t pts_ms = to_ms(pts_90k);

    if (!audio_config_sent_) {
        const std::uint8_t object_type = static_cast<std::uint8_t>(profile + 1);
        std::vector<std::byte> body;
        put_u8(body, 0xAF); // AAC audio flags
        put_u8(body, 0x00); // AACPacketType 0 = sequence header
        put_u8(body, static_cast<std::uint32_t>((object_type << 3) | ((freq_index >> 1) & 0x07)));
        put_u8(body, static_cast<std::uint32_t>(((freq_index & 0x01) << 7) | (channel_cfg << 3)));
        segmenter_.on_audio(make_message(8, pts_ms, std::move(body)));
        audio_config_sent_ = true;
    }

    std::vector<std::byte> body;
    put_u8(body, 0xAF); // AAC audio flags
    put_u8(body, 0x01); // AACPacketType 1 = raw
    body.insert(body.end(), adts.begin() + static_cast<std::ptrdiff_t>(header_len), adts.end());
    segmenter_.on_audio(make_message(8, pts_ms, std::move(body)));
}

} // namespace rtmp_server::hls
