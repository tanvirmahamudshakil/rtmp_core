#include "rtmp_server/hls/playlist.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rtmp_server::hls {

namespace {

// EXTINF durations are printed with 3 decimals: enough precision that
// accumulated drift stays well under a frame over a long live window.
std::string format_duration(std::chrono::milliseconds ms) {
    char buffer[32];
    const double seconds = static_cast<double>(ms.count()) / 1000.0;
    std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
    return buffer;
}

} // namespace

std::string build_media_playlist(std::span<const SegmentPtr> segments,
                                 const MediaPlaylistOptions& options) {
    // TARGETDURATION must be >= ceil(longest segment). Deriving it here
    // rather than trusting the configured value keeps the playlist valid
    // when a publisher's keyframe interval overshoots the target.
    std::uint32_t target = options.target_duration_seconds;
    for (const auto& segment : segments) {
        if (!segment) continue;
        const auto rounded =
            static_cast<std::uint32_t>((segment->duration.count() + 999) / 1000);
        target = std::max(target, rounded);
    }
    if (target == 0) target = 1;

    std::uint64_t media_sequence = 0;
    for (const auto& segment : segments) {
        if (segment) {
            media_sequence = segment->sequence;
            break;
        }
    }

    std::string out;
    out.reserve(256 + segments.size() * 64);
    out += "#EXTM3U\n";
    out += "#EXT-X-VERSION:" + std::to_string(options.version) + "\n";
    out += "#EXT-X-TARGETDURATION:" + std::to_string(target) + "\n";
    out += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(media_sequence) + "\n";
    if (options.discontinuity_sequence > 0) {
        out += "#EXT-X-DISCONTINUITY-SEQUENCE:" + std::to_string(options.discontinuity_sequence) + "\n";
    }
    if (options.emit_server_control) {
        // CAN-BLOCK-RELOAD=NO: no blocking playlist reload here, so a
        // compliant player polls at the TARGETDURATION cadence.
        out += "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=NO";
        if (options.hold_back_seconds > 0.0) {
            // RFC 8216bis: HOLD-BACK MUST be at least 3 x TARGETDURATION.
            const double hold_back =
                std::max(options.hold_back_seconds, 3.0 * static_cast<double>(target));
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.3f", hold_back);
            out += ",HOLD-BACK=" + std::string(buffer);
        }
        out += "\n";
    }

    for (const auto& segment : segments) {
        if (!segment) continue;
        // EXT-X-DISCONTINUITY precedes the segment it applies to (RFC 8216
        // 4.3.2.3) so the player resets its decoder before parsing it.
        if (segment->discontinuity) out += "#EXT-X-DISCONTINUITY\n";
        out += "#EXTINF:" + format_duration(segment->duration) + ",\n";
        out += options.segment_uri_prefix + segment->name + "\n";
    }

    if (options.ended) out += "#EXT-X-ENDLIST\n";
    return out;
}

std::string build_master_playlist(std::span<const Rendition> renditions) {
    std::vector<Rendition> sorted(renditions.begin(), renditions.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const Rendition& a, const Rendition& b) { return a.bandwidth < b.bandwidth; });

    std::string out;
    out += "#EXTM3U\n";
    out += "#EXT-X-VERSION:3\n";

    for (const auto& r : sorted) {
        out += "#EXT-X-STREAM-INF:BANDWIDTH=" + std::to_string(r.bandwidth);
        if (r.average_bandwidth > 0) {
            out += ",AVERAGE-BANDWIDTH=" + std::to_string(r.average_bandwidth);
        }
        if (r.width > 0 && r.height > 0) {
            out += ",RESOLUTION=" + std::to_string(r.width) + "x" + std::to_string(r.height);
        }
        if (r.frame_rate > 0.0) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.3f", r.frame_rate);
            out += ",FRAME-RATE=" + std::string(buffer);
        }
        if (!r.codecs.empty()) out += ",CODECS=\"" + r.codecs + "\"";
        if (!r.name.empty()) out += ",NAME=\"" + r.name + "\"";
        out += "\n";
        out += r.uri + "\n";
    }
    return out;
}

} // namespace rtmp_server::hls
