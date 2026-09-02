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

std::string format_seconds(double seconds) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
    return buffer;
}

// Two key descriptions are "the same key" for playlist purposes when a
// player would fetch and apply the identical thing; only then may the
// EXT-X-KEY tag be omitted before the next segment.
bool same_key(const EncryptionKeyInfoPtr& a, const EncryptionKeyInfoPtr& b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    return a->method == b->method && a->uri == b->uri && a->iv_hex == b->iv_hex &&
           a->key_format == b->key_format && a->key_format_versions == b->key_format_versions;
}

void append_key_tag(std::string& out, const EncryptionKeyInfoPtr& key) {
    if (!key || !key->encrypted()) {
        // RFC 8216 4.3.2.4: METHOD=NONE ends encryption for the segments that
        // follow, and must carry no other attribute.
        out += "#EXT-X-KEY:METHOD=NONE\n";
        return;
    }
    out += "#EXT-X-KEY:METHOD=" + key->method;
    out += ",URI=\"" + key->uri + "\"";
    if (!key->iv_hex.empty()) out += ",IV=" + key->iv_hex;
    if (!key->key_format.empty()) out += ",KEYFORMAT=\"" + key->key_format + "\"";
    if (!key->key_format_versions.empty()) {
        out += ",KEYFORMATVERSIONS=\"" + key->key_format_versions + "\"";
    }
    out += "\n";
}

void append_part_tag(std::string& out, const std::string& uri_prefix, const Part& part,
                     double part_target_seconds) {
    out += "#EXT-X-PART:DURATION=" + format_duration(part.duration);
    out += ",URI=\"" + uri_prefix + part.name + "\"";
    if (part.independent) out += ",INDEPENDENT=YES";
    out += "\n";
    (void)part_target_seconds;
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

    // EXT-X-PART and EXT-X-PRELOAD-HINT are version 9 tags; EXT-X-BYTERANGE
    // and EXT-X-I-FRAMES-ONLY need at least 4. Raising the declared version
    // here means a caller cannot emit a tag its own EXT-X-VERSION forbids.
    std::uint32_t version = options.version;
    if (options.low_latency) version = std::max<std::uint32_t>(version, 9);
    if (options.iframes_only) version = std::max<std::uint32_t>(version, 4);

    std::string out;
    out.reserve(256 + segments.size() * 96);
    out += "#EXTM3U\n";
    out += "#EXT-X-VERSION:" + std::to_string(version) + "\n";
    out += "#EXT-X-TARGETDURATION:" + std::to_string(target) + "\n";
    out += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(media_sequence) + "\n";
    if (options.discontinuity_sequence > 0) {
        out += "#EXT-X-DISCONTINUITY-SEQUENCE:" + std::to_string(options.discontinuity_sequence) + "\n";
    }
    if (options.iframes_only) out += "#EXT-X-I-FRAMES-ONLY\n";

    if (options.emit_server_control) {
        out += "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=";
        out += options.low_latency ? "YES" : "NO";
        if (options.hold_back_seconds > 0.0) {
            // RFC 8216bis: HOLD-BACK MUST be at least 3 x TARGETDURATION.
            const double hold_back =
                std::max(options.hold_back_seconds, 3.0 * static_cast<double>(target));
            out += ",HOLD-BACK=" + format_seconds(hold_back);
        }
        if (options.low_latency && options.part_target_seconds > 0.0) {
            // PART-HOLD-BACK MUST be at least 3 x PART-TARGET. A player uses
            // it to decide how far behind the live edge to start, so a value
            // below the floor makes it stall on every join.
            const double part_hold_back =
                std::max(options.part_hold_back_seconds, 3.0 * options.part_target_seconds);
            out += ",PART-HOLD-BACK=" + format_seconds(part_hold_back);
        }
        out += "\n";
    }

    if (options.low_latency && options.part_target_seconds > 0.0) {
        out += "#EXT-X-PART-INF:PART-TARGET=" + format_seconds(options.part_target_seconds) + "\n";
    }

    // Parts are only carried for the newest few segments: a player never
    // needs a part it could fetch as a whole segment instead, and every part
    // line is re-sent on each of the several playlist fetches per second that
    // low latency implies.
    std::size_t first_part_segment = 0;
    if (options.low_latency && segments.size() > options.part_window_segments) {
        first_part_segment = segments.size() - options.part_window_segments;
    }

    EncryptionKeyInfoPtr active_key;
    bool key_tag_written = false;

    std::size_t index = 0;
    for (const auto& segment : segments) {
        if (!segment) {
            ++index;
            continue;
        }
        // A trick-play playlist can only list segments whose I-frame prefix
        // was located; anything else has no byte range to advertise.
        if (options.iframes_only && segment->iframe_prefix_bytes == 0) {
            ++index;
            continue;
        }

        // EXT-X-KEY applies to every following segment until the next one, so
        // it is emitted only when the key actually changes.
        if (!key_tag_written || !same_key(active_key, segment->key)) {
            if (segment->key || key_tag_written) {
                append_key_tag(out, segment->key);
                key_tag_written = true;
            }
            active_key = segment->key;
        }

        // EXT-X-DISCONTINUITY precedes the segment it applies to (RFC 8216
        // 4.3.2.3) so the player resets its decoder before parsing it.
        if (segment->discontinuity) out += "#EXT-X-DISCONTINUITY\n";

        if (options.low_latency && index >= first_part_segment) {
            for (const auto& part : segment->parts) {
                if (part) append_part_tag(out, options.segment_uri_prefix, *part,
                                          options.part_target_seconds);
            }
        }

        out += "#EXTINF:" + format_duration(segment->duration) + ",\n";
        if (options.iframes_only) {
            // "<length>@<offset>" — the I-frame prefix always starts at 0,
            // because it has to include the program tables to be decodable.
            out += "#EXT-X-BYTERANGE:" + std::to_string(segment->iframe_prefix_bytes) + "@0\n";
        }
        out += options.segment_uri_prefix + segment->name + "\n";
        ++index;
    }

    if (options.low_latency && !options.iframes_only) {
        for (const auto& part : options.open_parts) {
            if (part) append_part_tag(out, options.segment_uri_prefix, *part,
                                      options.part_target_seconds);
        }
        if (!options.preload_hint_uri.empty()) {
            // A player issues this request before the part exists; the origin
            // holds it open until the bytes are produced. Only one PART hint
            // may be present, and it must name the very next part.
            out += "#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"" + options.segment_uri_prefix +
                   options.preload_hint_uri + "\"\n";
        }
    }

    if (options.ended) out += "#EXT-X-ENDLIST\n";
    return out;
}

std::string build_master_playlist(std::span<const Rendition> renditions) {
    std::vector<Rendition> sorted(renditions.begin(), renditions.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const Rendition& a, const Rendition& b) { return a.bandwidth < b.bandwidth; });

    const bool any_iframe_playlist =
        std::any_of(sorted.begin(), sorted.end(),
                    [](const Rendition& r) { return !r.iframe_uri.empty(); });

    std::string out;
    out += "#EXTM3U\n";
    // EXT-X-I-FRAME-STREAM-INF requires EXT-X-VERSION 4.
    out += any_iframe_playlist ? "#EXT-X-VERSION:4\n" : "#EXT-X-VERSION:3\n";

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

    // Trick-play variants are listed after the playable ones. An
    // EXT-X-I-FRAME-STREAM-INF carries its URI as an attribute, not on the
    // following line, and must not advertise a FRAME-RATE.
    for (const auto& r : sorted) {
        if (r.iframe_uri.empty()) continue;
        out += "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=" + std::to_string(r.bandwidth);
        if (r.width > 0 && r.height > 0) {
            out += ",RESOLUTION=" + std::to_string(r.width) + "x" + std::to_string(r.height);
        }
        if (!r.codecs.empty()) out += ",CODECS=\"" + r.codecs + "\"";
        out += ",URI=\"" + r.iframe_uri + "\"\n";
    }
    return out;
}

} // namespace rtmp_server::hls
