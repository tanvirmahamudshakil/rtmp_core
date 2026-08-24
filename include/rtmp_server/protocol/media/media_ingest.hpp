#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/clock.hpp"
#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/hevc/hevc.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"

namespace rtmp_server::protocol::media {

// FLV/RTMP audio "sound format" nibble (top 4 bits of the first audio tag
// byte). Only AAC (10) carries a sequence header this phase needs to parse;
// other codecs (PCM, MP3, Speex, ...) are still tracked in AudioTrackInfo
// but their payload is opaque to this component.
enum class AudioCodec : std::uint8_t {
    Unknown = 0xFF,
    LinearPcmPlatformEndian = 0,
    Adpcm = 1,
    Mp3 = 2,
    LinearPcmLittleEndian = 3,
    Nellymoser16kHzMono = 4,
    Nellymoser8kHzMono = 5,
    Nellymoser = 6,
    G711ALaw = 7,
    G711MuLaw = 8,
    Aac = 10,
    Speex = 11,
    Mp3_8kHz = 14,
};

// FLV/RTMP video "codec id" nibble (bottom 4 bits of the first video tag
// byte). AVC (7) and, now, HEVC (12) carry a sequence header this phase
// understands.
//
// CodecID 12 for HEVC is a pre-"Enhanced RTMP" convention used by several
// live vendors (notably in China) before the FLV spec had an official HEVC
// codec id: legacy FLV/RTMP tags just reuse AVC's tag layout (frame-type
// nibble, then a packet-type byte, then either an HEVCDecoderConfigurationRecord
// or Annex-B-convertible NALUs) with codec id 12 instead of 7. This is
// distinct from the newer "Enhanced RTMP" extended tag header (FourCC
// `hvc1`, ExVideoTagHeader, PacketTypeSequenceStart, ...), which is not
// implemented here -- see classify_video_tag's comment.
enum class VideoCodec : std::uint8_t {
    Unknown = 0xFF,
    Sorenson_H263 = 2,
    ScreenVideo = 3,
    On2_VP6 = 4,
    On2_VP6WithAlpha = 5,
    ScreenVideo2 = 6,
    Avc = 7,
    Hevc = 12,
};

// FLV video frame type nibble (top 4 bits of the first video tag byte).
enum class VideoFrameType : std::uint8_t {
    Unknown = 0,
    KeyFrame = 1,          // for AVC, a seekable frame (IDR)
    InterFrame = 2,        // non-seekable frame
    DisposableInterFrame = 3, // H.263 only
    GeneratedKeyFrame = 4, // reserved for server use
    VideoInfoOrCommand = 5,
};

// AVCPacketType (AVC video payload, byte after the frame-type/codec-id byte).
enum class AvcPacketType : std::uint8_t {
    SequenceHeader = 0, // AVCDecoderConfigurationRecord (SPS/PPS)
    Nalu = 1,
    EndOfSequence = 2,
};

// AACPacketType (AAC audio payload, byte after the sound-format/rate/size/type byte).
enum class AacPacketType : std::uint8_t {
    SequenceHeader = 0, // AudioSpecificConfig
    Raw = 1,
};

// Enhanced RTMP (github.com/veovera/enhanced-rtmp) ExVideoTagHeader video
// PacketType nibble (low 4 bits of the first tag byte, present only when
// that byte's top bit -- IsExVideoHeader -- is set). Confidence note: this
// server has no test vectors for Enhanced RTMP to validate against, so this
// layout follows the publicly documented "Enhanced RTMP v1" spec from
// memory; if a real Enhanced-RTMP encoder's bytes don't decode as expected,
// re-check this against the spec's ExVideoTagHeader section before assuming
// the encoder is wrong.
enum class ExVideoPacketType : std::uint8_t {
    SequenceStart = 0,   // VideoFourCc-specific decoder configuration record
    CodedFrames = 1,     // composition-time-prefixed coded frame (like classic AVC)
    SequenceEnd = 2,
    CodedFramesX = 3,    // coded frame with composition time implicitly 0 (no CTS field)
    Metadata = 4,
    Mpeg2TsSequenceStart = 5,
};

// VideoFourCc values ExVideoTagHeader carries (4 ASCII bytes, big-endian as
// a uint32) identifying the packaging in use. Only HEVC is understood here.
inline constexpr std::uint32_t kVideoFourCcHevc =
    (std::uint32_t{'h'} << 24) | (std::uint32_t{'v'} << 16) | (std::uint32_t{'c'} << 8) | std::uint32_t{'1'};

// Parsed contents of one AVCDecoderConfigurationRecord (ISO 14496-15), as
// carried by an AVCPacketType::SequenceHeader video tag. SPS/PPS are kept in
// their raw NALU-payload form (no start code / no length prefix) so a later
// phase (Playback/FLV recording) can re-wrap them as needed.
struct AvcSequenceHeader {
    std::uint8_t profile = 0;
    std::uint8_t profile_compatibility = 0;
    std::uint8_t level = 0;
    std::uint8_t nalu_length_size = 4; // lengthSizeMinusOne + 1
    std::vector<std::vector<std::byte>> sps_list;
    std::vector<std::vector<std::byte>> pps_list;
};

// Parsed AudioSpecificConfig (ISO 14496-3), as carried by an
// AacPacketType::SequenceHeader audio tag. Stored both as the decoded fields
// and as the raw bytes (a later phase re-sending the sequence header to a
// new subscriber needs the exact original bytes, not a re-encoding).
struct AacSequenceHeader {
    std::uint8_t object_type = 0;      // audioObjectType
    std::uint8_t sampling_frequency_index = 0;
    std::uint32_t sampling_frequency = 0; // resolved from the index (0 if reserved/unknown)
    std::uint8_t channel_configuration = 0;
    std::vector<std::byte> raw; // exact AudioSpecificConfig bytes
};

// Running per-stream counters. Deliberately minimal — richer bitrate/health
// metrics belong to observability (docs/rtmp_promot.md "Observability"), not
// this ingest-parsing component.
struct MediaStats {
    std::uint64_t audio_message_count = 0;
    std::uint64_t video_message_count = 0;
    std::uint64_t metadata_message_count = 0;
    std::uint64_t rejected_message_count = 0;
    std::uint64_t audio_bytes = 0;
    std::uint64_t video_bytes = 0;
    std::uint32_t keyframe_count = 0;
    std::optional<std::uint32_t> last_audio_timestamp;
    std::optional<std::uint32_t> last_video_timestamp;
    std::optional<std::uint32_t> last_keyframe_timestamp;
};

// Everything MediaIngest knows about one publishing stream: retained
// sequence headers, live stats, and the codec identifiers observed so far.
// Kept as a distinct per-stream object (referenced by stream_key) rather
// than folded into commands::StreamRegistration/StreamRegistry directly, so
// this component stays independently testable without pulling in
// CommandSession/StreamRegistry (mirrors how ChunkDecoder/HandshakeSession
// don't reach into each other) — see docs/media-ingest.md "Where per-stream
// media state lives" for the full reasoning. A later phase is free to have
// StreamRegistry hold a handle to (or pointer into) this state instead of
// duplicating it.
struct StreamMediaState {
    AudioCodec audio_codec = AudioCodec::Unknown;
    VideoCodec video_codec = VideoCodec::Unknown;
    std::optional<AvcSequenceHeader> avc_sequence_header;
    std::optional<AacSequenceHeader> aac_sequence_header;
    // Set once an Enhanced RTMP (hvc1) PacketTypeSequenceStart tag has been
    // parsed. Legacy CodecID-12 HEVC (see VideoCodec::Hevc's comment) has no
    // sequence-header parsing yet -- only classification -- so this is only
    // ever populated via the Enhanced path.
    std::optional<::rtmp_server::media::hevc::HevcDecoderConfig> hevc_sequence_header;
    bool seen_keyframe = false;
    MediaStats stats;
};

// Consumes chunk::RtmpMessage values of type Audio(8)/Video(9)/Amf0Data(18)
// for streams that are currently Publishing, per docs/rtmp_promot.md "Phase
// 5: Media Ingest". Pure parsing/bookkeeping component: no sockets, no
// io_uring, no AMF0 wire I/O of its own beyond what's needed to read
// @setDataFrame/onMetaData payloads that were already decoded by
// chunk::ChunkDecoder — matches the separation used by
// protocol::commands::CommandSession (docs/architecture.md "Architectural
// Separation").
class MediaIngest {
public:
    // Called once per audio/video/metadata message. Returns an error
    // (ErrorCode::MalformedAmf for metadata, a new MalformedChunk-category
    // rejection for audio/video short/truncated tags) rather than throwing;
    // callers should still count the rejection (this call already does, via
    // MediaStats::rejected_message_count) and move on rather than tear down
    // the connection, matching how ChunkDecoder handles bad input.
    core::Result<void> on_audio_message(std::string_view stream_key, const chunk::RtmpMessage& message);
    core::Result<void> on_video_message(std::string_view stream_key, const chunk::RtmpMessage& message);
    core::Result<void> on_metadata_message(std::string_view stream_key, const chunk::RtmpMessage& message);

    // Drops all retained state for a stream (call on unpublish/disconnect).
    void remove_stream(std::string_view stream_key);

    [[nodiscard]] const StreamMediaState* find(std::string_view stream_key) const;
    [[nodiscard]] std::size_t stream_count() const { return streams_.size(); }

private:
    StreamMediaState& state_for(std::string_view stream_key);

    std::unordered_map<std::string, StreamMediaState> streams_;
};

// Exposed for testing/reuse by a later phase (FLV recording needs the same
// AVCDecoderConfigurationRecord/AudioSpecificConfig parsers to rebuild an
// FLV file header).
[[nodiscard]] core::Result<AvcSequenceHeader> parse_avc_sequence_header(std::span<const std::byte> payload);
[[nodiscard]] core::Result<AacSequenceHeader> parse_aac_sequence_header(std::span<const std::byte> payload);

// Pure (no state mutation) classification of one raw FLV video/audio tag,
// factored out of MediaIngest::on_video_message/on_audio_message so other
// components (protocol::commands::LiveFanout's keyframe/sequence-header
// detection, docs/v2_promot.md PHASE 3) reuse the exact same bit-parsing
// instead of re-deriving it a second time. Returns std::nullopt for an
// empty/too-short payload (the caller decides how to treat that — MediaIngest
// counts it as rejected, LiveFanout simply treats it as "not a keyframe/not a
// sequence header").
struct VideoTagInfo {
    VideoFrameType frame_type = VideoFrameType::Unknown;
    VideoCodec codec = VideoCodec::Unknown;
    // Set when codec == Avc, or codec == Hevc (legacy CodecID 12 -- see
    // VideoCodec::Hevc's comment; the packet-type byte has the same layout
    // as AVC's in that convention), and the payload carries the byte.
    std::optional<AvcPacketType> avc_packet_type;
    // True when the first tag byte's top bit (IsExVideoHeader) was set --
    // i.e. this is an Enhanced RTMP ExVideoTagHeader tag, not a classic FLV
    // one. `codec`/`avc_packet_type` above are only meaningful for the
    // classic form; use `fourcc`/`ex_packet_type` instead for an enhanced tag.
    bool enhanced = false;
    // VideoFourCc from the ExVideoTagHeader (valid only when `enhanced` and
    // the payload was long enough to carry it). codec is set to
    // VideoCodec::Hevc as a convenience when fourcc == kVideoFourCcHevc, so
    // existing frame_type/keyframe logic keyed on `codec != Avc` keeps
    // working unchanged for the enhanced path.
    std::uint32_t fourcc = 0;
    std::optional<ExVideoPacketType> ex_packet_type;
};
struct AudioTagInfo {
    AudioCodec codec = AudioCodec::Unknown;
    std::optional<AacPacketType> aac_packet_type; // set only when codec == Aac and payload carries the byte
};

[[nodiscard]] std::optional<VideoTagInfo> classify_video_tag(std::span<const std::byte> payload);
[[nodiscard]] std::optional<AudioTagInfo> classify_audio_tag(std::span<const std::byte> payload);

} // namespace rtmp_server::protocol::media
