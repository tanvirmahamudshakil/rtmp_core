#include "rtmp_server/transcoding/native/hls_playlist.hpp"

#include <algorithm>
#include <charconv>

namespace rtmp_server::transcoding::native {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Extracts the value of attribute `key` from an HLS attribute list, e.g.
// BANDWIDTH=1280000,RESOLUTION=1280x720. Values may be quoted.
std::string_view attribute(std::string_view line, std::string_view key) {
    std::size_t pos = 0;
    while ((pos = line.find(key, pos)) != std::string_view::npos) {
        // Ensure it is a full attribute name (preceded by ':' or ',').
        const bool boundary = pos == 0 || line[pos - 1] == ',' || line[pos - 1] == ':' || line[pos - 1] == ' ';
        const std::size_t after = pos + key.size();
        if (boundary && after < line.size() && line[after] == '=') {
            std::size_t start = after + 1;
            if (start < line.size() && line[start] == '"') {
                ++start;
                const std::size_t end = line.find('"', start);
                return line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            }
            const std::size_t end = line.find(',', start);
            return line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        }
        pos = after;
    }
    return {};
}

std::uint64_t to_u64(std::string_view s) {
    std::uint64_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

double to_double(std::string_view s) {
    // std::from_chars<double> is not universally available; parse simply.
    try {
        return std::stod(std::string(s));
    } catch (...) {
        return 0.0;
    }
}

// Splits text into lines, dropping a trailing empty line.
std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            lines.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return lines;
}

} // namespace

std::string resolve_url(std::string_view base, std::string_view reference) {
    reference = trim(reference);
    if (reference.empty()) return std::string(base);
    // Absolute URL.
    if (reference.find("://") != std::string_view::npos) return std::string(reference);

    // Find the scheme://host boundary in base.
    const std::size_t scheme = base.find("://");
    const std::size_t host_start = scheme == std::string_view::npos ? 0 : scheme + 3;
    const std::size_t path_start = base.find('/', host_start);
    const std::string_view origin =
        path_start == std::string_view::npos ? base : base.substr(0, path_start);

    if (!reference.empty() && reference.front() == '/') {
        return std::string(origin) + std::string(reference); // absolute path
    }
    // Relative path: strip the last path segment of base.
    const std::size_t last_slash = base.rfind('/');
    const std::string_view dir =
        last_slash == std::string_view::npos || last_slash < host_start ? origin
                                                                        : base.substr(0, last_slash);
    return std::string(dir) + "/" + std::string(reference);
}

bool is_master_playlist(std::string_view text) {
    return text.find("#EXT-X-STREAM-INF") != std::string_view::npos;
}

std::vector<HlsVariant> parse_master_playlist(std::string_view text, std::string_view base_url) {
    std::vector<HlsVariant> variants;
    const auto lines = lines_of(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("#EXT-X-STREAM-INF", 0) != 0) continue;
        HlsVariant variant;
        variant.bandwidth = to_u64(attribute(lines[i], "BANDWIDTH"));
        const std::string_view resolution = attribute(lines[i], "RESOLUTION");
        if (const std::size_t x = resolution.find('x'); x != std::string_view::npos) {
            variant.width = static_cast<std::uint32_t>(to_u64(resolution.substr(0, x)));
            variant.height = static_cast<std::uint32_t>(to_u64(resolution.substr(x + 1)));
        }
        // The URI is the next non-comment, non-empty line.
        for (std::size_t j = i + 1; j < lines.size(); ++j) {
            if (lines[j].empty()) continue;
            if (lines[j].front() == '#') continue;
            variant.uri = resolve_url(base_url, lines[j]);
            break;
        }
        if (!variant.uri.empty()) variants.push_back(std::move(variant));
    }
    return variants;
}

HlsMediaPlaylist parse_media_playlist(std::string_view text, std::string_view base_url) {
    HlsMediaPlaylist playlist;
    const auto lines = lines_of(text);
    double pending_duration = 0;
    bool pending_discontinuity = false;
    std::uint64_t sequence = 0;
    bool sequence_seen = false;

    for (std::string_view line : lines) {
        if (line.empty()) continue;
        if (line.rfind("#EXT-X-TARGETDURATION:", 0) == 0) {
            playlist.target_duration = to_double(line.substr(22));
        } else if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
            playlist.media_sequence = to_u64(line.substr(22));
            sequence = playlist.media_sequence;
            sequence_seen = true;
        } else if (line.rfind("#EXT-X-DISCONTINUITY", 0) == 0 &&
                   line.rfind("#EXT-X-DISCONTINUITY-SEQUENCE", 0) != 0) {
            pending_discontinuity = true;
        } else if (line.rfind("#EXTINF:", 0) == 0) {
            pending_duration = to_double(line.substr(8));
        } else if (line == "#EXT-X-ENDLIST") {
            playlist.endlist = true;
        } else if (line.front() != '#') {
            HlsSegmentRef seg;
            seg.uri = resolve_url(base_url, line);
            seg.duration = pending_duration;
            seg.discontinuity = pending_discontinuity;
            seg.sequence = sequence_seen ? sequence : playlist.segments.size();
            ++sequence;
            playlist.segments.push_back(std::move(seg));
            pending_duration = 0;
            pending_discontinuity = false;
        }
    }
    return playlist;
}

std::string select_variant(const std::vector<HlsVariant>& variants, std::uint64_t bitrate_cap) {
    const HlsVariant* best = nullptr;
    for (const auto& v : variants) {
        if (bitrate_cap != 0 && v.bandwidth > bitrate_cap) continue;
        if (best == nullptr || v.bandwidth > best->bandwidth) best = &v;
    }
    if (best == nullptr) {
        // Nothing under the cap: fall back to the highest overall.
        for (const auto& v : variants)
            if (best == nullptr || v.bandwidth > best->bandwidth) best = &v;
    }
    return best ? best->uri : std::string{};
}

} // namespace rtmp_server::transcoding::native
