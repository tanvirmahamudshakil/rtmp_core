#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rtmp_server/core/buffer.hpp"

namespace rtmp_server::hls {

// EXT-X-KEY description for a segment (RFC 8216 4.3.2.4).
//
// Held per-segment rather than per-playlist so a key rotation emits a new
// EXT-X-KEY in the middle of the live window and older segments keep
// pointing at the key that actually decrypts them — which is the whole point
// of rotating, and impossible to express with one playlist-wide key.
struct EncryptionKeyInfo {
    std::string method;   // "AES-128"; empty means the segment is in the clear
    std::string uri;      // key delivery URI, as it should appear in the playlist
    std::string iv_hex;   // "0x..." explicit IV; empty lets the player derive it
    std::string key_format;
    std::string key_format_versions;

    [[nodiscard]] bool encrypted() const noexcept { return !method.empty() && method != "NONE"; }
};
using EncryptionKeyInfoPtr = std::shared_ptr<const EncryptionKeyInfo>;

// One Low-Latency HLS partial segment (EXT-X-PART, RFC 8216bis 4.4.4.9).
//
// A part is a prefix slice of a segment that is published the moment it is
// produced, so a player can start playing roughly one part behind the encoder
// instead of one whole segment behind it. Parts are served as their own URIs
// (rather than byte ranges of a not-yet-complete segment) because a byte-range
// request against a growing resource is exactly what a shared reverse cache
// cannot serve — and the cache is what makes this origin's audience size work
// at all (docs/hls.md "Maximum-scale public mode").
struct Part {
    std::uint64_t segment_sequence = 0; // the segment this part belongs to
    std::uint32_t index = 0;            // 0-based position within that segment
    std::string name;                   // e.g. "segment-42.3.ts"
    core::SharedBuffer data;
    std::chrono::milliseconds duration{0};
    // True when the part starts with a keyframe, so a player may begin
    // playback at it. Advertised as INDEPENDENT=YES.
    bool independent = false;

    [[nodiscard]] std::size_t size_bytes() const noexcept { return data.size(); }
};
using PartPtr = std::shared_ptr<const Part>;

// One finished HLS media segment.
//
// `data` is a core::SharedBuffer (shared_ptr<const vector<byte>>), the same
// immutable-shared-storage pattern LiveFanout uses for media frames
// (docs/v2_promot.md 3.8). Every concurrent HTTP viewer requesting this
// segment receives a shared_ptr copy of the identical bytes — a segment is
// never deep-copied per viewer, no matter how many players fetch it.
struct Segment {
    std::uint64_t sequence = 0;               // EXT-X-MEDIA-SEQUENCE number
    std::string name;                          // e.g. "segment-42.ts"
    core::SharedBuffer data;
    std::chrono::milliseconds duration{0};
    // True when this segment does not continue the previous one's timeline
    // (codec header change, timestamp rollover, publisher reconnect). Drives
    // EXT-X-DISCONTINUITY in the playlist.
    bool discontinuity = false;

    // Low-Latency HLS: the parts this segment was published as, in order.
    // Empty when low latency is disabled, and the segment is then exactly
    // what it always was.
    std::vector<PartPtr> parts;

    // AES-128 key this segment's bytes are encrypted under. Null when the
    // segment is in the clear.
    EncryptionKeyInfoPtr key;

    // Byte length, from offset 0, of the leading run that contains the
    // program tables plus the segment's first keyframe — i.e. an
    // independently decodable I-frame prefix. Drives EXT-X-BYTERANGE in the
    // EXT-X-I-FRAMES-ONLY (trick play) playlist. 0 means no I-frame was
    // located, and the segment is omitted from that playlist.
    std::uint64_t iframe_prefix_bytes = 0;

    [[nodiscard]] std::size_t size_bytes() const noexcept { return data.size(); }
};

using SegmentPtr = std::shared_ptr<const Segment>;

} // namespace rtmp_server::hls
