#include "rtmp_server/dash/mpd.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace rtmp_server::dash {

namespace {

std::string format_seconds_duration(double seconds) {
    // ISO 8601 duration, e.g. "PT6.000S". DASH uses this form for every
    // *Duration/*Delay/*Depth/*Period attribute.
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "PT%.3fS", seconds);
    return buffer;
}

std::string escape_attribute(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

// "$Number$" -> "$Number%0Nd$" padding is deliberately not used: unpadded
// $Number$ is legal per 5.3.9.4.4 and matches the plain decimal names the
// segmenter/store already give every segment (hls does the same for its own
// sequence-numbered names).
std::string replace_rep(std::string_view tmpl, const std::string& rep_id) {
    std::string out;
    out.reserve(tmpl.size());
    std::size_t pos = 0;
    while (pos < tmpl.size()) {
        const auto found = tmpl.find("{rep}", pos);
        if (found == std::string_view::npos) {
            out.append(tmpl.substr(pos));
            break;
        }
        out.append(tmpl.substr(pos, found - pos));
        out += rep_id;
        pos = found + 5;
    }
    return out;
}

} // namespace

std::string build_mpd(std::span<const Representation> representations, const MpdOptions& options) {
    std::vector<Representation> sorted(representations.begin(), representations.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const Representation& a, const Representation& b) {
        return a.bandwidth < b.bandwidth;
    });

    // Group into AdaptationSets by mime_type, preserving first-seen order so
    // the output is deterministic across calls with the same input.
    std::vector<std::string> mime_order;
    std::unordered_map<std::string, std::vector<const Representation*>> by_mime;
    for (const auto& rep : sorted) {
        auto& bucket = by_mime[rep.mime_type];
        if (bucket.empty() &&
            std::find(mime_order.begin(), mime_order.end(), rep.mime_type) == mime_order.end()) {
            mime_order.push_back(rep.mime_type);
        }
        bucket.push_back(&rep);
    }

    std::string out;
    out += R"(<?xml version="1.0" encoding="UTF-8"?>)";
    out += "\n<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" ";
    out += "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\" ";
    out += options.is_static ? "type=\"static\" " : "type=\"dynamic\" ";
    if (!options.is_static) {
        // A live MPD carries no fixed availabilityStartTime here: every
        // representation addresses segments by $Number$ relative to
        // @startNumber, not by wall-clock time, so players never need one to
        // resolve a URL — the same reasoning HLS's server-relative sequence
        // numbers rely on.
        if (options.minimum_update_period_seconds > 0.0) {
            out += "minimumUpdatePeriod=\"" +
                   format_seconds_duration(options.minimum_update_period_seconds) + "\" ";
        }
        if (options.time_shift_buffer_depth_seconds > 0.0) {
            out += "timeShiftBufferDepth=\"" +
                   format_seconds_duration(options.time_shift_buffer_depth_seconds) + "\" ";
        }
        if (options.suggested_presentation_delay_seconds > 0.0) {
            out += "suggestedPresentationDelay=\"" +
                   format_seconds_duration(options.suggested_presentation_delay_seconds) + "\" ";
        }
    } else {
        out += "mediaPresentationDuration=\"" +
               format_seconds_duration(options.total_duration_seconds) + "\" ";
    }
    out += "minBufferTime=\"PT2.000S\">\n";

    out += "  <Period id=\"0\" start=\"PT0S\">\n";

    for (const auto& mime : mime_order) {
        const auto& reps = by_mime.at(mime);
        if (reps.empty()) continue;
        const bool is_audio = mime.rfind("audio/", 0) == 0;
        out += "    <AdaptationSet contentType=\"" + std::string(is_audio ? "audio" : "video") +
               "\" mimeType=\"" + escape_attribute(mime) + "\" segmentAlignment=\"true\">\n";
        // Common attributes live on the SegmentTemplate; per-representation
        // templates below only override initialization/media (the actual
        // path differs per rendition).
        out += "      <SegmentTemplate timescale=\"" + std::to_string(options.timescale) +
               "\" duration=\"" + std::to_string(options.segment_duration) + "\" startNumber=\"" +
               std::to_string(options.start_number) + "\"/>\n";

        for (const auto* rep : reps) {
            out += "      <Representation id=\"" + escape_attribute(rep->id) + "\" bandwidth=\"" +
                   std::to_string(rep->bandwidth) + "\"";
            if (!rep->codecs.empty()) out += " codecs=\"" + escape_attribute(rep->codecs) + "\"";
            if (!is_audio && rep->width > 0 && rep->height > 0) {
                out += " width=\"" + std::to_string(rep->width) + "\" height=\"" +
                       std::to_string(rep->height) + "\"";
                if (rep->frame_rate > 0.0) {
                    char buffer[32];
                    std::snprintf(buffer, sizeof(buffer), "%.3f", rep->frame_rate);
                    out += " frameRate=\"" + std::string(buffer) + "\"";
                }
            }
            if (is_audio && rep->audio_sampling_rate > 0) {
                out += " audioSamplingRate=\"" + std::to_string(rep->audio_sampling_rate) + "\"";
            }
            out += ">\n";
            out += "        <SegmentTemplate timescale=\"" + std::to_string(options.timescale) +
                   "\" duration=\"" + std::to_string(options.segment_duration) +
                   "\" startNumber=\"" + std::to_string(options.start_number) + "\" initialization=\"" +
                   escape_attribute(replace_rep(rep->init_template, rep->id)) + "\" media=\"" +
                   escape_attribute(replace_rep(rep->media_template, rep->id)) + "\"/>\n";
            out += "      </Representation>\n";
        }

        out += "    </AdaptationSet>\n";
    }

    out += "  </Period>\n";
    out += "</MPD>\n";
    return out;
}

} // namespace rtmp_server::dash
