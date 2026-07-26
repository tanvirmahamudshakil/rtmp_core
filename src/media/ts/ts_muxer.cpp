#include "rtmp_server/media/ts/ts_muxer.hpp"

#include <algorithm>

namespace rtmp_server::media::ts {

namespace {

constexpr std::uint8_t kStreamIdVideo = 0xE0;
constexpr std::uint8_t kStreamIdAudio = 0xC0;

constexpr std::uint8_t kStreamTypeH264 = 0x1B;
constexpr std::uint8_t kStreamTypeAacAdts = 0x0F;

void push_u8(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
}

void push_u16(std::vector<std::byte>& out, std::uint32_t v) {
    push_u8(out, v >> 8);
    push_u8(out, v);
}

void push_u32(std::vector<std::byte>& out, std::uint32_t v) {
    push_u8(out, v >> 24);
    push_u8(out, v >> 16);
    push_u8(out, v >> 8);
    push_u8(out, v);
}

// Encodes a 33-bit PTS/DTS into the 5-byte PES timestamp layout. `prefix` is
// 0b0010 (PTS only), 0b0011 (PTS with DTS following) or 0b0001 (DTS).
void push_timestamp(std::vector<std::byte>& out, std::uint8_t prefix, std::uint64_t value) {
    const std::uint64_t v = value & 0x1FFFFFFFFULL; // 33 bits, wraps naturally
    push_u8(out, (static_cast<std::uint32_t>(prefix) << 4) |
                     static_cast<std::uint32_t>((v >> 30) & 0x07) | 0x01u);
    push_u16(out, static_cast<std::uint32_t>(((v >> 15) & 0x7FFF) << 1) | 0x01);
    push_u16(out, static_cast<std::uint32_t>((v & 0x7FFF) << 1) | 0x01);
}

} // namespace

std::uint32_t mpeg_crc32(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::byte b : data) {
        crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 24;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
        }
    }
    return crc;
}

void TsMuxer::write_psi_packet(std::vector<std::byte>& out, std::uint16_t pid,
                               std::uint8_t& continuity_counter, std::span<const std::byte> section) {
    const std::size_t start = out.size();

    push_u8(out, kSyncByte);
    // payload_unit_start_indicator = 1
    push_u8(out, 0x40 | ((pid >> 8) & 0x1F));
    push_u8(out, pid & 0xFF);
    // adaptation_field_control = 01 (payload only)
    push_u8(out, 0x10 | (continuity_counter & 0x0F));
    continuity_counter = (continuity_counter + 1) & 0x0F;

    push_u8(out, 0x00); // pointer_field: section starts immediately
    out.insert(out.end(), section.begin(), section.end());

    // A PSI packet is always exactly one TS packet; pad the remainder with
    // 0xFF stuffing (the standard value for unused PSI payload space).
    while (out.size() - start < kPacketSize) out.push_back(std::byte{0xFF});
}

void TsMuxer::write_program_tables(std::vector<std::byte>& out) {
    // ---- PAT ----
    {
        std::vector<std::byte> section;
        push_u8(section, 0x00); // table_id
        // section_syntax_indicator(1) '0'(1) reserved(2) section_length(12)
        // length = 5 (rest of header) + 4 (one program entry) + 4 (CRC) = 13
        push_u16(section, 0xB000 | 13);
        push_u16(section, 0x0001); // transport_stream_id
        push_u8(section, 0xC1);    // reserved(2) version(5)=0 current_next(1)=1
        push_u8(section, 0x00);    // section_number
        push_u8(section, 0x00);    // last_section_number
        push_u16(section, config_.program_number);
        push_u16(section, 0xE000 | (config_.pmt_pid & 0x1FFF));
        push_u32(section, mpeg_crc32(section));
        write_psi_packet(out, 0x0000, pat_cc_, section);
    }

    // ---- PMT ----
    {
        std::vector<std::byte> section;
        push_u8(section, 0x02); // table_id
        // 9 (rest of header) + 5 (video ES) + 5 (audio ES) + 4 (CRC) = 23
        push_u16(section, 0xB000 | 23);
        push_u16(section, config_.program_number);
        push_u8(section, 0xC1);
        push_u8(section, 0x00);
        push_u8(section, 0x00);
        // PCR is carried on the video PID (it is the stream with the
        // reliable, keyframe-anchored timebase).
        push_u16(section, 0xE000 | (config_.video_pid & 0x1FFF));
        push_u16(section, 0xF000); // program_info_length = 0

        push_u8(section, kStreamTypeH264);
        push_u16(section, 0xE000 | (config_.video_pid & 0x1FFF));
        push_u16(section, 0xF000); // ES_info_length = 0

        push_u8(section, kStreamTypeAacAdts);
        push_u16(section, 0xE000 | (config_.audio_pid & 0x1FFF));
        push_u16(section, 0xF000);

        push_u32(section, mpeg_crc32(section));
        write_psi_packet(out, config_.pmt_pid, pmt_cc_, section);
    }
}

void TsMuxer::write_pes_packets(std::vector<std::byte>& out, std::uint16_t pid,
                                std::uint8_t& continuity_counter, std::span<const std::byte> payload,
                                bool with_pcr, std::uint64_t pcr_90k, bool random_access) {
    std::size_t offset = 0;
    bool first = true;

    while (offset < payload.size()) {
        const std::size_t packet_start = out.size();
        const std::size_t remaining = payload.size() - offset;

        // An adaptation field is needed on the first packet when we carry a
        // PCR / random-access flag, and on any packet whose payload would
        // not fill the 184 available bytes (stuffing).
        const bool want_af_flags = first && (with_pcr || random_access);
        std::size_t af_len = 0; // total adaptation field bytes incl. length byte
        if (want_af_flags) {
            af_len = with_pcr ? 8 : 2; // len + flags [+ 6 PCR bytes]
        }

        std::size_t available = kPacketSize - 4 - af_len;
        std::size_t stuffing = 0;
        if (remaining < available) {
            stuffing = available - remaining;
            if (af_len == 0) {
                // Introduce a minimal adaptation field to hold the stuffing.
                // 1 stuffing byte => adaptation_field_length = 0 (no flags
                // byte); otherwise length byte + flags byte + filler.
                af_len = stuffing;
                stuffing = (af_len >= 2) ? af_len - 2 : 0;
            } else {
                af_len += stuffing;
            }
            available = kPacketSize - 4 - af_len;
        }

        const std::size_t payload_bytes = std::min(remaining, available);

        push_u8(out, kSyncByte);
        push_u8(out, (first ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
        push_u8(out, pid & 0xFF);
        const std::uint8_t afc = (af_len > 0 ? 0x30 : 0x10);
        push_u8(out, afc | (continuity_counter & 0x0F));
        continuity_counter = (continuity_counter + 1) & 0x0F;

        if (af_len > 0) {
            push_u8(out, static_cast<std::uint32_t>(af_len - 1));
            if (af_len >= 2) {
                std::uint8_t flags = 0;
                if (first && random_access) flags |= 0x40; // random_access_indicator
                if (first && with_pcr) flags |= 0x10;      // PCR_flag
                push_u8(out, flags);
                if (first && with_pcr) {
                    // PCR base is 33 bits at 90 kHz; extension (300ths) is 0.
                    const std::uint64_t base = pcr_90k & 0x1FFFFFFFFULL;
                    push_u8(out, static_cast<std::uint32_t>(base >> 25));
                    push_u8(out, static_cast<std::uint32_t>(base >> 17));
                    push_u8(out, static_cast<std::uint32_t>(base >> 9));
                    push_u8(out, static_cast<std::uint32_t>(base >> 1));
                    push_u8(out, static_cast<std::uint32_t>((base & 0x01) << 7) | 0x7E);
                    push_u8(out, 0x00);
                }
                for (std::size_t i = 0; i < stuffing; ++i) push_u8(out, 0xFF);
            }
        }

        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset),
                   payload.begin() + static_cast<std::ptrdiff_t>(offset + payload_bytes));
        offset += payload_bytes;
        first = false;

        // Defensive: every emitted packet must be exactly 188 bytes.
        while (out.size() - packet_start < kPacketSize) out.push_back(std::byte{0xFF});
    }
}

core::Result<void> TsMuxer::write_video(std::vector<std::byte>& out, std::span<const std::byte> annexb,
                                        std::uint64_t pts_90k, std::uint64_t dts_90k, bool keyframe) {
    if (annexb.empty()) {
        return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                           "empty H.264 access unit");
    }

    std::vector<std::byte> pes;
    pes.reserve(annexb.size() + 32);
    push_u8(pes, 0x00);
    push_u8(pes, 0x00);
    push_u8(pes, 0x01);
    push_u8(pes, kStreamIdVideo);

    const bool with_dts = (dts_90k != pts_90k);
    const std::size_t header_data_len = with_dts ? 10u : 5u;
    const std::size_t pes_payload_len = annexb.size() + 3 + header_data_len;
    // PES_packet_length is 16 bits. Video access units routinely exceed
    // 65535 bytes, and the standard permits 0 ("unbounded") for video on
    // its own PID — the TS packet framing then delimits the PES.
    push_u16(pes, pes_payload_len > 0xFFFF ? 0u : static_cast<std::uint32_t>(pes_payload_len));

    push_u8(pes, 0x80); // '10' marker, no scrambling, not priority
    push_u8(pes, with_dts ? 0xC0 : 0x80); // PTS_DTS_flags
    push_u8(pes, static_cast<std::uint32_t>(header_data_len));
    push_timestamp(pes, with_dts ? 0x03 : 0x02, pts_90k);
    if (with_dts) push_timestamp(pes, 0x01, dts_90k);

    pes.insert(pes.end(), annexb.begin(), annexb.end());

    write_pes_packets(out, config_.video_pid, video_cc_, pes, /*with_pcr=*/keyframe,
                      /*pcr_90k=*/dts_90k, /*random_access=*/keyframe);
    return {};
}

core::Result<void> TsMuxer::write_audio(std::vector<std::byte>& out, std::span<const std::byte> adts,
                                        std::uint64_t pts_90k) {
    if (adts.empty()) {
        return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                           "empty AAC access unit");
    }

    std::vector<std::byte> pes;
    pes.reserve(adts.size() + 16);
    push_u8(pes, 0x00);
    push_u8(pes, 0x00);
    push_u8(pes, 0x01);
    push_u8(pes, kStreamIdAudio);

    const std::size_t pes_payload_len = adts.size() + 3 + 5;
    if (pes_payload_len > 0xFFFF) {
        // Audio PES must carry a real length; an AAC AU group this large is
        // not something a conformant encoder produces.
        return core::Error(core::ErrorCode::MessageTooLarge, core::ErrorCategory::Protocol,
                           "AAC PES payload exceeds 65535 bytes");
    }
    push_u16(pes, static_cast<std::uint32_t>(pes_payload_len));
    push_u8(pes, 0x80);
    push_u8(pes, 0x80); // PTS only
    push_u8(pes, 5);
    push_timestamp(pes, 0x02, pts_90k);

    pes.insert(pes.end(), adts.begin(), adts.end());

    write_pes_packets(out, config_.audio_pid, audio_cc_, pes, /*with_pcr=*/false, /*pcr_90k=*/0,
                      /*random_access=*/false);
    return {};
}

} // namespace rtmp_server::media::ts
