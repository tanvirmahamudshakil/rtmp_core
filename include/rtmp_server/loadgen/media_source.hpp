#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rtmp_server::loadgen {

// Synthetic but structurally-valid H.264/AAC media generator for the Phase 7
// load tool (docs/v2_promot.md PHASE 7 "Publishing realistic H.264/AAC
// payload sizes").
//
// This deliberately produces REAL FLV tag bodies, not the two-byte stubs the
// pre-Phase-7 apps/load_bench used:
//
//   * Video tags carry a real FrameType/CodecID byte (0x17 keyframe / 0x27
//     inter, CodecID 7 = AVC), a real AVCPacketType, a 3-byte composition
//     time, and a length-prefixed NAL. The server's
//     protocol::media::classify_video_tag() therefore genuinely recognises
//     keyframes and sequence headers, which is what makes the GOP cache and
//     the slow-viewer keyframe-resume policy actually engage under load. A
//     two-byte stub payload exercises none of that.
//   * Frame SIZES follow the configured bitrate, keyframe interval and frame
//     rate, with keyframes ~8x the size of an inter frame — so the bytes on
//     the wire, the per-viewer queue accounting and the GOP cache byte
//     budget all see a realistic distribution rather than a flat one.
//   * Every generated frame embeds a verifiable pattern (see
//     verify_pattern()) so a viewer can detect corruption or mis-framing
//     byte-for-byte rather than merely counting bytes.
//
// It is NOT a real encoder: the NAL payload bytes are a deterministic
// pseudo-random pattern, not decodable video. That is intentional and
// sufficient — the server is a passthrough relay and never decodes pixels.
// Anything claiming to measure decode quality would need a real encoder and
// is out of scope for a transport load test.

struct MediaProfile {
    // Video
    std::uint32_t video_bitrate_bps = 2'500'000;
    std::uint32_t frames_per_second = 30;
    // Keyframe every N video frames (GOP length). 0 disables keyframes after
    // the first, which is a useful pathological case for the GOP cache.
    std::uint32_t keyframe_interval_frames = 60;

    // Audio
    std::uint32_t audio_bitrate_bps = 128'000;
    // AAC frames are 1024 samples; at 44.1 kHz that is ~43.07 frames/sec.
    std::uint32_t audio_sample_rate = 44'100;

    // Hard ceiling on a single generated video payload, so a misconfigured
    // bitrate cannot make the tool allocate without bound.
    std::size_t max_frame_bytes = 4u * 1024u * 1024u;
};

enum class FrameKind : std::uint8_t { VideoSequenceHeader, AudioSequenceHeader, VideoKeyframe, VideoInter, Audio };

struct GeneratedFrame {
    FrameKind kind = FrameKind::VideoInter;
    std::uint32_t timestamp_ms = 0;
    std::vector<std::byte> payload; // complete FLV tag body
};

[[nodiscard]] constexpr bool is_video(FrameKind kind) noexcept {
    return kind == FrameKind::VideoSequenceHeader || kind == FrameKind::VideoKeyframe ||
           kind == FrameKind::VideoInter;
}

// Deterministic, restartable media generator. Not thread-safe: one instance
// per simulated publisher.
class MediaSource {
public:
    explicit MediaSource(MediaProfile profile, std::uint64_t seed = 1);

    // The two sequence headers a fresh publisher must send before any media,
    // in the order a decoder needs them.
    [[nodiscard]] GeneratedFrame video_sequence_header() const;
    [[nodiscard]] GeneratedFrame audio_sequence_header() const;

    // Produces every frame (video and audio, interleaved by timestamp) whose
    // presentation time falls at or before `up_to_ms`, advancing internal
    // state. Returns them in non-decreasing timestamp order.
    [[nodiscard]] std::vector<GeneratedFrame> frames_until(std::uint32_t up_to_ms);

    // Resets timestamps and frame counters but keeps the profile — models a
    // publisher reconnect, which restarts the timeline from zero.
    void restart();

    [[nodiscard]] std::uint64_t video_frames_generated() const noexcept { return video_frames_; }
    [[nodiscard]] std::uint64_t audio_frames_generated() const noexcept { return audio_frames_; }
    [[nodiscard]] std::uint64_t bytes_generated() const noexcept { return bytes_generated_; }

    // ---- corruption detection ------------------------------------------------

    // True if `payload` is a well-formed frame produced by any MediaSource
    // with this seed: checks the FLV tag header bytes, the declared NAL/AAC
    // length against the actual payload length, and the embedded byte
    // pattern. A viewer calls this on every received media payload; any
    // false result is a corruption/framing bug in the server or the tool.
    [[nodiscard]] static bool verify_pattern(std::span<const std::byte> payload) noexcept;

private:
    // Fills `out` with the deterministic verification pattern for a frame
    // whose payload-region length is `length`.
    static void fill_pattern(std::vector<std::byte>& out, std::size_t length, std::uint8_t tag);

    [[nodiscard]] std::size_t next_video_payload_size(bool keyframe) const;

    MediaProfile profile_;
    std::uint64_t seed_;

    std::uint64_t video_frames_ = 0;
    std::uint64_t audio_frames_ = 0;
    std::uint64_t bytes_generated_ = 0;

    // Next frame indices still to be emitted.
    std::uint64_t next_video_index_ = 0;
    std::uint64_t next_audio_index_ = 0;
};

} // namespace rtmp_server::loadgen
