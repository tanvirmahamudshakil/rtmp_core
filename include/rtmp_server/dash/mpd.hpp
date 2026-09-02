#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// MPEG-DASH Media Presentation Description (MPD, ISO/IEC 23009-1) generation
// for one live stream.
//
// Pure string building over an already-decided representation list — no I/O,
// no locks — mirroring hls::build_media_playlist/build_master_playlist. The
// live profile used here is `urn:mpeg:dash:profile:isoff-live:2011` with
// SegmentTemplate/$Number$ addressing: every representation is cut on the
// same wall-clock cadence (the segmenter's target duration), so a single
// duration and start number describe the whole timeline without a
// SegmentTimeline's per-segment `<S>` entries. This is the same
// simplification HLS's fixed-cadence segmenter already makes; a publisher
// whose GOP length varies produces segments of slightly different real
// duration, same as it does for HLS, and players tolerate it the same way.
namespace rtmp_server::dash {

// One encoded representation (matches hls::Rendition's role).
struct Representation {
    std::string id;                 // stable across the live session
    std::uint64_t bandwidth = 0;    // bits/second
    std::string codecs;             // RFC 6381, e.g. "avc1.64001f,mp4a.40.2"
    std::string mime_type = "video/mp4"; // audio-only representations use "audio/mp4"
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double frame_rate = 0.0;
    std::uint32_t audio_sampling_rate = 0; // 0 omits the attribute (video-only)
    // Relative path templates. "$Number$" is replaced by the player per
    // ISO/IEC 23009-1 5.3.9.4.4.
    std::string init_template;    // e.g. "{rep}/init.m4p"
    std::string media_template;   // e.g. "{rep}/chunk-$Number$.m4s"
};

struct MpdOptions {
    // SegmentTemplate timescale. 90 kHz matches media::mp4::kVideoTimescale,
    // so segment durations translate directly without a second conversion.
    std::uint32_t timescale = 90000;
    // Nominal segment duration, in `timescale` units. Individual segments
    // may differ slightly (keyframe-cut, same as HLS); this is the value a
    // player uses to plan its buffer and request cadence.
    std::uint64_t segment_duration = 0;
    // $Number$ of the oldest segment still in the live window.
    std::uint64_t start_number = 0;
    // Suggests how far behind the live edge a player should start, in
    // seconds. Mirrors HLS's HOLD-BACK. 0 lets the player choose.
    double suggested_presentation_delay_seconds = 0.0;
    // Live window length, in seconds — how far back a player may seek.
    double time_shift_buffer_depth_seconds = 0.0;
    // Minimum time before the manifest may next change meaningfully; a
    // player is expected to re-fetch it at roughly this cadence, the DASH
    // analogue of HLS's TARGETDURATION-paced polling.
    double minimum_update_period_seconds = 0.0;
    // Static (VOD) presentation: @type="static", MPD@mediaPresentationDuration
    // set, no MinimumUpdatePeriod. False (the default) is a live @type="dynamic"
    // presentation.
    bool is_static = false;
    // Total duration, in seconds. Required when is_static is true; ignored
    // otherwise.
    double total_duration_seconds = 0.0;
};

// Builds one Period with one AdaptationSet per distinct mime_type
// (video/audio split automatically), each holding its Representations.
// Representations are emitted lowest-bandwidth-first within their
// AdaptationSet, matching hls::build_master_playlist's ordering.
[[nodiscard]] std::string build_mpd(std::span<const Representation> representations,
                                    const MpdOptions& options);

} // namespace rtmp_server::dash
