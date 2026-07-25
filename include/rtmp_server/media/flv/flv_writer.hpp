#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"

// Binary FLV (Flash Video) container encoding, per Adobe's
// "Video File Format Specification Version 10" (the FLV/F4V spec). This layer
// is pure byte-format code: it knows how to lay out an FLV header and FLV
// tags and nothing about io_uring, sockets, or recording orchestration —
// same architectural separation MediaIngest keeps (docs/media-ingest.md).
// The recording orchestration (async writer, bounded queue, finalization)
// lives one layer up in rtmp_server::recording (docs/flv-recording.md).
namespace rtmp_server::media::flv {

// FLV tag type IDs (TagType field of the 11-byte FLV tag header).
inline constexpr std::uint8_t kTagTypeAudio = 8;
inline constexpr std::uint8_t kTagTypeVideo = 9;
inline constexpr std::uint8_t kTagTypeScriptData = 18;

// Sizes of the fixed framing pieces.
inline constexpr std::size_t kFileHeaderSize = 9;      // "FLV" + version + flags + DataOffset
inline constexpr std::size_t kPreviousTagSize = 4;     // trailing size field after each tag (and 0 after header)
inline constexpr std::size_t kFileHeaderTotalSize = kFileHeaderSize + kPreviousTagSize; // 13
inline constexpr std::size_t kTagHeaderSize = 11;      // TagType(1)+DataSize(3)+Timestamp(3)+TimestampExt(1)+StreamID(3)

// TypeFlags bits in the FLV file header (byte 4).
inline constexpr std::uint8_t kFlagAudio = 0x04;
inline constexpr std::uint8_t kFlagVideo = 0x01;

// Encodes the 13-byte FLV file header: the 9-byte header ("FLV", version 1,
// TypeFlags, DataOffset=9) followed by PreviousTagSize0 = 0.
[[nodiscard]] std::array<std::byte, kFileHeaderTotalSize> encode_file_header(bool has_audio, bool has_video);

// Appends one complete FLV tag to `out`: the 11-byte tag header (TagType,
// DataSize=data.size(), Timestamp lower 24 bits + extended byte, StreamID=0),
// the tag data itself, then the trailing PreviousTagSize (11 + data.size()).
// Timestamps larger than 24 bits are carried in the TimestampExtended byte,
// exactly as the FLV spec requires (so a >4.6-hour recording stays valid).
void append_tag(std::vector<std::byte>& out, std::uint8_t tag_type, std::span<const std::byte> data,
                std::uint32_t timestamp);

// Big-endian IEEE-754 encoding of an AMF0 Number's 8 value bytes. Exposed so
// the recorder can patch the onMetaData duration/filesize placeholders in
// place at finalize without re-encoding the whole metadata tag.
[[nodiscard]] std::array<std::byte, 8> encode_double_be(double value);

// Values written into the onMetaData ECMA array. Sensible AAC/AVC defaults;
// duration and filesize are written as placeholders (0) and patched at
// finalize once their real values are known (see build_onmetadata_tag).
struct OnMetaData {
    double duration = 0.0;         // seconds; patched at finalize
    double width = 0.0;
    double height = 0.0;
    double framerate = 0.0;
    double videocodecid = 7.0;     // 7 = AVC
    double audiocodecid = 10.0;    // 10 = AAC
    double audiosamplerate = 0.0;
    double audiosamplesize = 16.0;
    bool stereo = true;
    double filesize = 0.0;         // bytes; patched at finalize
};

// A fully-encoded onMetaData script-data tag plus the offsets (relative to
// the first byte of the returned tag) of the 8-byte duration and filesize
// value fields, so the recorder can rewrite just those doubles at finalize.
struct MetadataTag {
    std::vector<std::byte> bytes;
    std::size_t duration_value_offset = 0;
    std::size_t filesize_value_offset = 0;
};

// Builds the onMetaData tag (AMF0 String "onMetaData" + ECMA array of the
// OnMetaData fields), fully framed as an FLV script-data tag at `timestamp`.
[[nodiscard]] MetadataTag build_onmetadata_tag(const OnMetaData& meta, std::uint32_t timestamp = 0);

// ---- Read-back / inspection ----------------------------------------------
// Minimal FLV parser used by the flv_inspector CLI and by tests to verify a
// written file byte-for-byte (no real media player is available on this
// host, same deferral prior phases used — see docs/phase6-checklist.md).

struct ParsedTag {
    std::uint8_t type = 0;
    std::uint32_t data_size = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t previous_tag_size = 0; // the size field that followed this tag
    std::size_t data_offset = 0;         // byte offset of the tag data within the file
};

struct ParsedFlv {
    std::uint8_t version = 0;
    bool has_audio = false;
    bool has_video = false;
    std::uint32_t data_offset = 0;         // DataOffset field from the header
    std::uint32_t previous_tag_size0 = 0;  // must be 0 in a valid file
    std::vector<ParsedTag> tags;
};

// Parses an entire FLV byte buffer. Fails (ErrorCode::MalformedChunk,
// ErrorCategory::Storage) on a bad signature, truncated header, or a tag
// whose DataSize/PreviousTagSize runs past the buffer — never throws, never
// reads out of bounds.
[[nodiscard]] core::Result<ParsedFlv> parse_flv(std::span<const std::byte> data);

} // namespace rtmp_server::media::flv
