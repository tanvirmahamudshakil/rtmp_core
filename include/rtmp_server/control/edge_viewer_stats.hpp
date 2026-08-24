#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rtmp_server::control {

// One reading of the cache edge's delivery accounting.
//
// Why this exists at all: the origin cannot count HLS viewers. Playlists and
// segments are collapsed by the local Varnish cache (deploy/varnish/
// streamforge.vcl gives a media playlist a 1s TTL), so a thousand players
// polling one link produce roughly one origin request per second — and
// HlsHttpHandler's own per-link session count therefore reports ~1 no matter
// how many people are watching. Varnish does see every request, including
// hits, which is what deploy/viewer-estimator/viewer_estimator.py consumes
// (varnishncsa) to publish per-link active playback-session counts.
//
// The admin panel used to be the only consumer of that file, fetching it
// straight from the browser and adding it to the API's numbers itself. That
// left the server's own API reporting RTMP subscribers only — zero for a
// purely HLS audience — and made the panel's numbers disappear entirely
// whenever the browser could not reach the file. Reading it here makes the
// real per-link viewer count part of what the server reports.
struct EdgeViewerSnapshot {
    // False when the file is missing, unparseable, or older than its own
    // measurement window (the estimator rewrites every 2s). Consumers fall
    // back to origin-side counting rather than reporting a stale number.
    bool fresh = false;
    // Key is "application/stream", exactly as the estimator emits it: the
    // stream component of the URL the player actually fetched. For a
    // source-transcode job that is a rendition output name, not the job's
    // master name.
    std::unordered_map<std::string, std::uint64_t> viewers;
    std::unordered_map<std::string, std::uint64_t> bytes_total;
    std::unordered_map<std::string, std::uint64_t> bitrate_bps;
    // Pre-aggregated by the estimator over the union of sessions, so a
    // player mid-ABR-switch (briefly visible under two rendition keys)
    // counts once here even though summing `viewers` would count it twice.
    std::uint64_t total_viewers = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t total_bitrate_bps = 0;
};

// Parses one estimator document. Exposed (and pure) so the parsing and
// staleness rules can be tested without a filesystem.
[[nodiscard]] EdgeViewerSnapshot parse_edge_viewer_stats(
    std::string_view document, std::chrono::system_clock::time_point now,
    std::chrono::seconds staleness_grace = std::chrono::seconds(10));

// Tuning for EdgeViewerStats. At namespace scope (not nested) because the
// constructor below defaults it: a nested type's default member initializers
// are not usable in a default argument of the enclosing class, which clang --
// the compiler scripts/install-linux.sh builds with -- rejects outright.
// EdgeViewerStats::Options still names it through the member alias.
struct EdgeViewerStatsOptions {
    // Where viewer_estimator.py writes. Empty disables the reader entirely
    // (every lookup reports "not fresh"), which is what a deployment without
    // the cache edge wants.
    std::string path = "/var/www/streamforge/internal/viewer_estimate.json";
    std::chrono::milliseconds refresh_interval{1000};
    std::chrono::seconds staleness_grace{10};
    // Refuse to read an implausibly large document rather than letting one
    // bad file balloon the server's memory.
    std::size_t max_bytes = 8u * 1024u * 1024u;
};

// Thread-safe, cached reader of the estimator's output file. Every accessor
// re-reads at most once per refresh interval, so the management API can call
// it per request without touching the disk per request.
class EdgeViewerStats {
public:
    using Options = EdgeViewerStatsOptions;

    explicit EdgeViewerStats(Options options = {});

    [[nodiscard]] EdgeViewerSnapshot snapshot();

    // Active playback sessions on exactly one link key ("application/stream").
    // Returns 0 when the edge reading is unavailable; check snapshot().fresh
    // when the caller needs to distinguish "nobody watching" from "unknown".
    [[nodiscard]] std::uint64_t viewers_for(std::string_view application, std::string_view stream);

    // Sum across several stream keys under one application — a source job's
    // rendition outputs plus its own name. A session that is switching
    // renditions can appear under two keys inside the estimator's 20s
    // window and would be counted twice here; use total_viewers for a
    // deduplicated server-wide figure.
    [[nodiscard]] std::uint64_t viewers_for_any(std::string_view application,
                                                std::span<const std::string> streams);
    // Same aggregation for delivered bytes and current bitrate, so a link's
    // bandwidth figures come from the same measurement as its viewers.
    [[nodiscard]] std::uint64_t bytes_for_any(std::string_view application,
                                              std::span<const std::string> streams);
    [[nodiscard]] std::uint64_t bitrate_for_any(std::string_view application,
                                                std::span<const std::string> streams);

private:
    void refresh_locked();

    Options options_;
    std::mutex mutex_;
    EdgeViewerSnapshot cached_;
    std::chrono::steady_clock::time_point last_read_{};
    bool ever_read_ = false;
};

} // namespace rtmp_server::control
