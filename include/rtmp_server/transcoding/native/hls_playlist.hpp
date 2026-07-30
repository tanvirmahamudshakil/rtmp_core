#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rtmp_server::transcoding::native {

// One media-playlist segment reference.
struct HlsSegmentRef {
    std::string uri;      // absolute, resolved against the playlist URL
    double duration = 0;  // EXTINF seconds
    std::uint64_t sequence = 0; // media sequence number (for de-duplication)
    bool discontinuity = false; // EXT-X-DISCONTINUITY precedes this segment
};

// A parsed HLS media playlist.
struct HlsMediaPlaylist {
    std::vector<HlsSegmentRef> segments;
    double target_duration = 0;
    std::uint64_t media_sequence = 0;
    bool endlist = false; // true = VOD/finished, false = live (keep polling)
};

// One variant from a master (multivariant) playlist.
struct HlsVariant {
    std::string uri;          // absolute
    std::uint64_t bandwidth = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Resolves a possibly-relative URI reference against a base URL (the playlist's
// own URL). Handles absolute URLs, absolute paths and relative paths; pure.
[[nodiscard]] std::string resolve_url(std::string_view base, std::string_view reference);

// True if the text looks like a master playlist (contains EXT-X-STREAM-INF).
[[nodiscard]] bool is_master_playlist(std::string_view text);

// Parses a master playlist, resolving each variant URI against `base_url`.
[[nodiscard]] std::vector<HlsVariant> parse_master_playlist(std::string_view text,
                                                            std::string_view base_url);

// Parses a media playlist, resolving each segment URI against `base_url`.
[[nodiscard]] HlsMediaPlaylist parse_media_playlist(std::string_view text,
                                                    std::string_view base_url);

// Picks the highest-bandwidth variant at or below `bitrate_cap` (0 = no cap;
// then the highest overall). Returns empty string if there are no variants.
[[nodiscard]] std::string select_variant(const std::vector<HlsVariant>& variants,
                                         std::uint64_t bitrate_cap);

} // namespace rtmp_server::transcoding::native
