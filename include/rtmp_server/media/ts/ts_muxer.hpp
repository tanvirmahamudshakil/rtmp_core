#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"

// Minimal MPEG-2 Transport Stream multiplexer, sufficient for HLS
// passthrough of H.264 video + AAC audio (ISO/IEC 13818-1).
//
// Why MPEG-TS rather than CMAF/fMP4 — see docs/hls.md "Container choice".
// In short: TS segments are self-contained (no init segment to coordinate,
// no box-level state to keep consistent across a publisher reconnect), and
// TS is the universally supported HLS container. The cost is ~4% framing
// overhead, which is acceptable for a first HLS implementation.
//
// This muxer performs NO transcoding. Video must already be Annex B and
// audio must already be ADTS-framed; both are copied through byte-for-byte.
namespace rtmp_server::media::ts {

inline constexpr std::size_t kPacketSize = 188;
inline constexpr std::uint8_t kSyncByte = 0x47;

// The 90 kHz clock MPEG-TS PTS/DTS values are expressed in.
inline constexpr std::uint64_t kClockHz = 90000;

struct TsMuxerConfig {
    std::uint16_t pmt_pid = 0x1000;
    std::uint16_t video_pid = 0x0100;
    std::uint16_t audio_pid = 0x0101;
    std::uint16_t program_number = 1;
};

// Stateful across a whole stream: continuity counters must advance
// monotonically per PID for the life of the stream, not per segment.
class TsMuxer {
public:
    explicit TsMuxer(TsMuxerConfig config = {}) : config_(config) {}

    // Emits PAT + PMT. Every segment must begin with these so that each
    // segment is independently decodable when a player joins mid-stream or
    // a CDN serves segments out of order.
    void write_program_tables(std::vector<std::byte>& out);

    // Appends an H.264 access unit (Annex B, start-code prefixed) as a PES
    // packet on the video PID. `keyframe` sets random_access_indicator and
    // emits a PCR, which is what lets a player start at this point.
    core::Result<void> write_video(std::vector<std::byte>& out, std::span<const std::byte> annexb,
                                   std::uint64_t pts_90k, std::uint64_t dts_90k, bool keyframe);

    // Appends one or more ADTS-framed AAC frames as a PES packet on the
    // audio PID.
    core::Result<void> write_audio(std::vector<std::byte>& out, std::span<const std::byte> adts,
                                   std::uint64_t pts_90k);

    // Resets continuity counters — used only when the elementary streams
    // genuinely restart (publisher reconnect), alongside an
    // EXT-X-DISCONTINUITY in the playlist.
    void reset_continuity() noexcept {
        video_cc_ = 0;
        audio_cc_ = 0;
        pat_cc_ = 0;
        pmt_cc_ = 0;
    }

    [[nodiscard]] const TsMuxerConfig& config() const noexcept { return config_; }

private:
    // Splits `payload` (a complete PES packet) across 188-byte TS packets.
    void write_pes_packets(std::vector<std::byte>& out, std::uint16_t pid, std::uint8_t& continuity_counter,
                           std::span<const std::byte> payload, bool with_pcr, std::uint64_t pcr_90k,
                           bool random_access);
    // Emits one PSI section (PAT/PMT) in a single TS packet with a pointer field.
    void write_psi_packet(std::vector<std::byte>& out, std::uint16_t pid, std::uint8_t& continuity_counter,
                          std::span<const std::byte> section);

    TsMuxerConfig config_;
    std::uint8_t video_cc_ = 0;
    std::uint8_t audio_cc_ = 0;
    std::uint8_t pat_cc_ = 0;
    std::uint8_t pmt_cc_ = 0;
};

// MPEG-2 systems CRC-32 (poly 0x04C11DB7, init 0xFFFFFFFF, MSB-first, no
// final inversion). Exposed for tests that validate generated PSI sections.
[[nodiscard]] std::uint32_t mpeg_crc32(std::span<const std::byte> data);

} // namespace rtmp_server::media::ts
