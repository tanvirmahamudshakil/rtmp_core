#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rtmp_server/hls/segment.hpp"

// HLS playlist (.m3u8) generation, per RFC 8216.
//
// Pure string building over an already-decided segment list — no I/O, no
// locks — so playlist correctness is exhaustively unit-testable and the
// caller decides when/where to publish the result atomically.
namespace rtmp_server::hls {

struct MediaPlaylistOptions {
    // EXT-X-TARGETDURATION: must be an integer >= the longest segment
    // duration in the playlist, rounded up (RFC 8216 4.3.3.1). A player that
    // sees a segment longer than this may stall, so we compute it from the
    // actual segments rather than trusting configuration alone.
    std::uint32_t target_duration_seconds = 4;
    // EXT-X-VERSION. 3 is the minimum for float EXTINF durations; 6 when
    // EXT-X-DISCONTINUITY-SEQUENCE is present alongside other v6 features.
    std::uint32_t version = 3;
    // Emitted as EXT-X-DISCONTINUITY-SEQUENCE when non-zero: the number of
    // discontinuities that have already scrolled out of the live window.
    // Without it a player that joins late mis-associates its decoder reset.
    std::uint64_t discontinuity_sequence = 0;
    // Appends EXT-X-ENDLIST (VOD / finished stream).
    bool ended = false;
    // Relative URI prefix prepended to each segment name.
    std::string segment_uri_prefix;
    // Emit EXT-X-SERVER-CONTROL. Players that predate the tag ignore it.
    // CAN-BLOCK-RELOAD=NO is always set when emitted: this origin does not
    // implement blocking playlist reload, so a compliant player re-fetches at
    // the TARGETDURATION cadence instead of polling aggressively — the HLS
    // analogue of Wowza's "client idle frequency" control, and the main lever
    // for origin request rate at a large audience.
    bool emit_server_control = true;
    // HOLD-BACK seconds advertised in EXT-X-SERVER-CONTROL: how far back from
    // the live edge a player begins. 0 omits the attribute (player uses its
    // 3 x TARGETDURATION default). A value below 3 x TARGETDURATION is invalid
    // per RFC 8216bis and is clamped up.
    double hold_back_seconds = 0.0;
};

// Builds a media playlist. TARGETDURATION is raised automatically if any
// segment is longer than the configured value, keeping the playlist
// RFC-conformant even when a keyframe interval overshoots.
[[nodiscard]] std::string build_media_playlist(std::span<const SegmentPtr> segments,
                                               const MediaPlaylistOptions& options);

// One variant in a master playlist.
struct Rendition {
    std::string uri;             // relative path to the media playlist
    std::uint64_t bandwidth = 0; // EXT-X-STREAM-INF BANDWIDTH (bits/second), required
    std::uint64_t average_bandwidth = 0; // optional; omitted when 0
    std::string codecs;          // RFC 6381, e.g. "avc1.64001f,mp4a.40.2"
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double frame_rate = 0.0;
    std::string name;            // human label (informational)
};

// Builds a master (multivariant) playlist listing each rendition.
// Renditions are emitted lowest-bandwidth-first so a player without
// measured throughput starts conservatively.
[[nodiscard]] std::string build_master_playlist(std::span<const Rendition> renditions);

} // namespace rtmp_server::hls
