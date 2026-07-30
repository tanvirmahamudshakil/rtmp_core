#include "rtmp_server/media/ts/ts_demuxer.hpp"

#include <algorithm>

#include "rtmp_server/media/h264/avc.hpp"

namespace rtmp_server::media::ts {

namespace {

constexpr std::uint8_t kStreamTypeH264 = 0x1B;
constexpr std::uint8_t kStreamTypeAacAdts = 0x0F;

std::uint8_t u8(std::span<const std::byte> b, std::size_t i) {
    return static_cast<std::uint8_t>(b[i]);
}

// Decodes a 33-bit PTS/DTS from the 5-byte PES timestamp layout (the inverse of
// ts_muxer's push_timestamp): bits are carried at [3:1] of byte 0 and [15:1] of
// the two following 16-bit words, each with a marker bit.
std::uint64_t read_timestamp(std::span<const std::byte> p) {
    const std::uint64_t b0 = (u8(p, 0) >> 1) & 0x07;
    const std::uint64_t b1 = (static_cast<std::uint64_t>(u8(p, 1)) << 7) | (u8(p, 2) >> 1);
    const std::uint64_t b2 = (static_cast<std::uint64_t>(u8(p, 3)) << 7) | (u8(p, 4) >> 1);
    return (b0 << 30) | (b1 << 15) | b2;
}

// True if the Annex B access unit contains an IDR NAL (random access point).
bool contains_idr(std::span<const std::byte> annexb) {
    // Walk start codes (00 00 01 or 00 00 00 01) and inspect each NAL's type.
    std::size_t i = 0;
    const std::size_t n = annexb.size();
    while (i + 3 < n) {
        const bool sc3 = u8(annexb, i) == 0 && u8(annexb, i + 1) == 0 && u8(annexb, i + 2) == 1;
        const bool sc4 = i + 4 < n && u8(annexb, i) == 0 && u8(annexb, i + 1) == 0 &&
                         u8(annexb, i + 2) == 0 && u8(annexb, i + 3) == 1;
        if (sc4) {
            if ((u8(annexb, i + 4) & 0x1F) == h264::kNalTypeIdr) return true;
            i += 4;
        } else if (sc3) {
            if ((u8(annexb, i + 3) & 0x1F) == h264::kNalTypeIdr) return true;
            i += 3;
        } else {
            ++i;
        }
    }
    return false;
}

} // namespace

void TsDemuxer::reset() noexcept {
    pmt_pid_ = 0xFFFF;
    video_pid_ = 0xFFFF;
    audio_pid_ = 0xFFFF;
    pmt_known_ = false;
    video_ = PesAssembly{};
    audio_ = PesAssembly{};
    partial_.clear();
}

core::Result<void> TsDemuxer::feed(std::span<const std::byte> ts_bytes) {
    // Prepend any partial packet left over from the previous feed.
    std::vector<std::byte> buffer;
    std::span<const std::byte> data = ts_bytes;
    if (!partial_.empty()) {
        buffer.reserve(partial_.size() + ts_bytes.size());
        buffer.insert(buffer.end(), partial_.begin(), partial_.end());
        buffer.insert(buffer.end(), ts_bytes.begin(), ts_bytes.end());
        partial_.clear();
        data = buffer;
    }

    std::size_t i = 0;
    while (i + kPacketSize <= data.size()) {
        if (u8(data, i) != kSyncByte) {
            // Lost alignment: resynchronise to the next sync byte.
            ++i;
            continue;
        }
        if (auto r = process_packet(data.subspan(i, kPacketSize)); !r) return r;
        i += kPacketSize;
    }
    // Stash the trailing bytes that don't form a full packet.
    if (i < data.size()) partial_.assign(data.begin() + static_cast<std::ptrdiff_t>(i), data.end());
    return {};
}

core::Result<void> TsDemuxer::process_packet(std::span<const std::byte> packet) {
    const bool pusi = (u8(packet, 1) & 0x40) != 0;
    const std::uint16_t pid =
        static_cast<std::uint16_t>(((u8(packet, 1) & 0x1F) << 8) | u8(packet, 2));
    const std::uint8_t afc = (u8(packet, 3) >> 4) & 0x03;

    std::size_t payload_offset = 4;
    if (afc == 0x02) return {}; // adaptation field only, no payload
    if (afc == 0x03) {
        const std::size_t af_len = u8(packet, 4);
        payload_offset = 5 + af_len;
    }
    if (payload_offset >= packet.size()) return {};
    std::span<const std::byte> payload = packet.subspan(payload_offset);

    if (pid == 0x0000) {
        if (pusi && !payload.empty()) {
            const std::size_t pointer = u8(payload, 0);
            if (1 + pointer < payload.size()) parse_pat(payload.subspan(1 + pointer));
        }
        return {};
    }
    if (pmt_known_ && pid == pmt_pid_) {
        if (pusi && !payload.empty()) {
            const std::size_t pointer = u8(payload, 0);
            if (1 + pointer < payload.size()) parse_pmt(payload.subspan(1 + pointer));
        }
        return {};
    }

    PesAssembly* asm_state = nullptr;
    if (pid == video_pid_) asm_state = &video_;
    else if (pid == audio_pid_) asm_state = &audio_;
    if (asm_state == nullptr) return {};

    if (pusi) {
        finish_pes(*asm_state); // a new PES starts: close the previous one
        begin_pes(*asm_state, payload);
    } else if (asm_state->has_data) {
        asm_state->data.insert(asm_state->data.end(), payload.begin(), payload.end());
    }
    return {};
}

void TsDemuxer::parse_pat(std::span<const std::byte> section) {
    if (section.size() < 12) return;
    const std::size_t section_length = ((u8(section, 1) & 0x0F) << 8) | u8(section, 2);
    const std::size_t end = std::min(section.size(), 3 + section_length);
    // Program entries start at byte 8, four bytes each, minus the trailing CRC.
    for (std::size_t i = 8; i + 4 <= end - 4; i += 4) {
        const std::uint16_t program = static_cast<std::uint16_t>((u8(section, i) << 8) | u8(section, i + 1));
        const std::uint16_t pid =
            static_cast<std::uint16_t>(((u8(section, i + 2) & 0x1F) << 8) | u8(section, i + 3));
        if (program != 0) { // program 0 is the network PID, not a PMT
            pmt_pid_ = pid;
            pmt_known_ = true;
            break; // first program is enough for single-program HLS/TS
        }
    }
}

void TsDemuxer::parse_pmt(std::span<const std::byte> section) {
    if (section.size() < 16) return;
    const std::size_t section_length = ((u8(section, 1) & 0x0F) << 8) | u8(section, 2);
    const std::size_t end = std::min(section.size(), 3 + section_length);
    const std::size_t program_info_length = ((u8(section, 10) & 0x0F) << 8) | u8(section, 11);
    std::size_t i = 12 + program_info_length;
    while (i + 5 <= end - 4) {
        const std::uint8_t stream_type = u8(section, i);
        const std::uint16_t pid =
            static_cast<std::uint16_t>(((u8(section, i + 1) & 0x1F) << 8) | u8(section, i + 2));
        const std::size_t es_info_length = ((u8(section, i + 3) & 0x0F) << 8) | u8(section, i + 4);
        if (stream_type == kStreamTypeH264 && video_pid_ == 0xFFFF) video_pid_ = pid;
        else if (stream_type == kStreamTypeAacAdts && audio_pid_ == 0xFFFF) audio_pid_ = pid;
        i += 5 + es_info_length;
    }
}

void TsDemuxer::begin_pes(PesAssembly& asm_state, std::span<const std::byte> payload) {
    // Require the PES start code 00 00 01 and enough header to read the length
    // and optional PTS/DTS.
    if (payload.size() < 9 || u8(payload, 0) != 0x00 || u8(payload, 1) != 0x00 ||
        u8(payload, 2) != 0x01) {
        asm_state.has_data = false;
        return;
    }
    const std::uint8_t pts_dts_flags = (u8(payload, 7) >> 6) & 0x03;
    const std::size_t header_data_length = u8(payload, 8);
    const std::size_t es_offset = 9 + header_data_length;
    if (es_offset > payload.size()) {
        asm_state.has_data = false;
        return;
    }

    std::uint64_t pts = 0;
    std::uint64_t dts = 0;
    if ((pts_dts_flags & 0x02) != 0 && payload.size() >= 14) {
        pts = read_timestamp(payload.subspan(9, 5));
        dts = pts;
        if (pts_dts_flags == 0x03 && payload.size() >= 19) dts = read_timestamp(payload.subspan(14, 5));
    }

    asm_state.pts_90k = pts;
    asm_state.dts_90k = dts;
    asm_state.data.assign(payload.begin() + static_cast<std::ptrdiff_t>(es_offset), payload.end());
    asm_state.has_data = true;
}

void TsDemuxer::finish_pes(PesAssembly& asm_state) {
    if (!asm_state.has_data || asm_state.data.empty()) {
        asm_state.has_data = false;
        return;
    }
    if (&asm_state == &video_ && video_handler_) {
        video_handler_(asm_state.data, asm_state.pts_90k, asm_state.dts_90k,
                       contains_idr(asm_state.data));
    } else if (&asm_state == &audio_ && audio_handler_) {
        audio_handler_(asm_state.data, asm_state.pts_90k);
    }
    asm_state.data.clear();
    asm_state.has_data = false;
}

void TsDemuxer::flush() {
    finish_pes(video_);
    finish_pes(audio_);
}

} // namespace rtmp_server::media::ts
