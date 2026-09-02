#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rtmp_server/hls/segment.hpp"

// HLS playlist (.m3u8) generation, per RFC 8216 and the Low-Latency HLS
// additions of RFC 8216bis.
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
    // EXT-X-DISCONTINUITY-SEQUENCE is present alongside other v6 features;
    // 9 is required once EXT-X-PART/EXT-X-SKIP appear, and low_latency
    // raises it automatically.
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
    // With low_latency off, CAN-BLOCK-RELOAD=NO is set: this origin then does
    // not implement blocking playlist reload, so a compliant player re-fetches
    // at the TARGETDURATION cadence instead of polling aggressively — the HLS
    // analogue of Wowza's "client idle frequency" control, and the main lever
    // for origin request rate at a large audience.
    bool emit_server_control = true;
    // HOLD-BACK seconds advertised in EXT-X-SERVER-CONTROL: how far back from
    // the live edge a player begins. 0 omits the attribute (player uses its
    // 3 x TARGETDURATION default). A value below 3 x TARGETDURATION is invalid
    // per RFC 8216bis and is clamped up.
    double hold_back_seconds = 0.0;

    // --- Low-Latency HLS -------------------------------------------------
    // Emits EXT-X-PART-INF, per-part EXT-X-PART lines, PART-HOLD-BACK and
    // CAN-BLOCK-RELOAD=YES, and raises EXT-X-VERSION to at least 9.
    bool low_latency = false;
    // EXT-X-PART-INF PART-TARGET: the maximum part duration, in seconds.
    double part_target_seconds = 0.0;
    // PART-HOLD-BACK. RFC 8216bis requires at least 3 x PART-TARGET; a
    // smaller value is clamped up.
    double part_hold_back_seconds = 0.0;
    // Parts belonging to segments older than this many segments back from the
    // live edge are omitted. Only the newest few segments need parts; keeping
    // every part in a long window inflates the playlist a player refetches
    // several times a second.
    std::size_t part_window_segments = 3;
    // Parts of the segment currently being produced, in order. These appear
    // after the last complete segment, with no EXTINF of their own.
    std::span<const PartPtr> open_parts;
    // URI of the next part that does not exist yet, advertised as
    // EXT-X-PRELOAD-HINT so a player can issue the request before the part is
    // produced. Empty omits the tag.
    std::string preload_hint_uri;

    // --- Trick play ------------------------------------------------------
    // Renders an EXT-X-I-FRAMES-ONLY playlist: one EXT-X-BYTERANGE entry per
    // segment covering its I-frame prefix, and no full segment URIs.
    bool iframes_only = false;
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
    // Relative path to this rendition's EXT-X-I-FRAMES-ONLY playlist. When
    // set, the master playlist also carries an EXT-X-I-FRAME-STREAM-INF entry
    // so a player can scrub without downloading full segments. Empty omits it.
    std::string iframe_uri;
};

// Builds a master (multivariant) playlist listing each rendition.
// Renditions are emitted lowest-bandwidth-first so a player without
// measured throughput starts conservatively.
[[nodiscard]] std::string build_master_playlist(std::span<const Rendition> renditions);

} // namespace rtmp_server::hls
