#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/aac/adts.hpp"
#include "rtmp_server/media/video_dimensions.hpp"

// Fragmented MP4 (ISO/IEC 14496-12) / CMAF (ISO/IEC 23000-19) multiplexer.
//
// Why this exists alongside media::ts::TsMuxer: MPEG-TS segments cannot carry
// Low-Latency HLS partial segments usefully (a TS "part" still has to repeat
// PAT/PMT and cannot be byte-addressed as a CMAF chunk), and MPEG-DASH has no
// TS profile worth shipping. Both features need the same container, so this
// muxer is the shared foundation: one init segment per rendition, then a
// stream of `moof`+`mdat` chunks that concatenate into a CMAF segment.
//
// Like TsMuxer this performs NO transcoding. Video samples must already be
// length-prefixed AVCC/HVCC exactly as RTMP/FLV delivers them (fMP4 wants the
// same layout, so unlike the TS path there is no Annex B conversion at all),
// and audio samples must be raw AAC access units with no ADTS header — the
// AudioSpecificConfig lives in the init segment's `esds` instead.
namespace rtmp_server::media::mp4 {

// Movie-level timescale written to `mvhd`. Only used for the (always zero,
// because this is live) movie duration, so its value is arbitrary.
inline constexpr std::uint32_t kMovieTimescale = 1000;
// Video track timescale. 90 kHz keeps the fMP4 and MPEG-TS timelines
// numerically identical, so the segmenter computes PTS/DTS once for both.
inline constexpr std::uint32_t kVideoTimescale = 90000;

inline constexpr std::uint32_t kVideoTrackId = 1;
inline constexpr std::uint32_t kAudioTrackId = 2;

enum class VideoCodec : std::uint8_t {
    None,
    H264, // `avc1` sample entry, `avcC` configuration box
    Hevc, // `hvc1` sample entry, `hvcC` configuration box
};

// Everything the init segment (`ftyp`+`moov`) needs. The decoder
// configuration records are stored verbatim: FLV already delivers a valid
// AVCDecoderConfigurationRecord/HEVCDecoderConfigurationRecord, and an fMP4
// `avcC`/`hvcC` box body is byte-identical to it, so re-serialising the
// parsed form would only add a way to get it wrong.
struct Fmp4InitConfig {
    VideoCodec video_codec = VideoCodec::None;
    std::vector<std::byte> video_decoder_config; // raw avcC / hvcC box body
    VideoDimensions video_dimensions;

    bool has_audio = false;
    aac::AudioSpecificConfig audio_config;
    std::vector<std::byte> audio_specific_config; // raw AudioSpecificConfig bytes

    [[nodiscard]] bool has_video() const noexcept { return video_codec != VideoCodec::None; }
};

// One coded sample (access unit) in a fragment.
struct Fmp4Sample {
    std::span<const std::byte> data; // AVCC/HVCC sample, or a raw AAC frame
    std::uint32_t duration = 0;      // in the track's timescale
    std::int32_t composition_offset = 0; // PTS - DTS, track timescale
    bool keyframe = false;               // sets the `trun` sync-sample flags
};

// A fragment's samples for one track, plus the decode time its first sample
// starts at (written to `tfdt`, so a player can seek/join without replaying
// earlier fragments).
struct Fmp4TrackFragment {
    std::uint64_t base_decode_time = 0;
    std::vector<Fmp4Sample> samples;

    [[nodiscard]] bool empty() const noexcept { return samples.empty(); }
};

// Stateful only in its fragment sequence number, which ISO/IEC 14496-12
// requires to increase across a track's fragments.
class Fmp4Muxer {
public:
    Fmp4Muxer() = default;

    // Builds `ftyp` + `moov`. Fails when the config declares neither a video
    // nor an audio track, or when a declared track has no decoder config.
    [[nodiscard]] core::Result<std::vector<std::byte>> init_segment(const Fmp4InitConfig& config) const;

    // Appends one CMAF chunk (`moof` + `mdat`) covering both tracks. Either
    // fragment may be empty (audio-only or video-only chunk) but not both.
    // Sample payload bytes are copied into `out` exactly once.
    [[nodiscard]] core::Result<void> write_fragment(std::vector<std::byte>& out,
                                                    const Fmp4TrackFragment& video,
                                                    const Fmp4TrackFragment& audio,
                                                    std::uint32_t audio_timescale);

    // Emits a `styp` segment-type box. CMAF segments start with one; HLS
    // players tolerate its absence but DASH `$Number$` segments should carry
    // it, so the caller decides per profile.
    static void write_styp(std::vector<std::byte>& out);

    // Resets the fragment sequence counter. Only for a genuine timeline
    // restart, alongside the playlist's discontinuity.
    void reset() noexcept { sequence_number_ = 0; }

    [[nodiscard]] std::uint32_t next_sequence_number() const noexcept { return sequence_number_ + 1; }

private:
    std::uint32_t sequence_number_ = 0;
};

// RFC 6381 codecs string for one track, e.g. "avc1.64001f" or "mp4a.40.2".
// Empty when the configuration record is too short to derive it.
[[nodiscard]] std::string video_codec_string(VideoCodec codec, std::span<const std::byte> decoder_config);
[[nodiscard]] std::string audio_codec_string(const aac::AudioSpecificConfig& config);

} // namespace rtmp_server::media::mp4
