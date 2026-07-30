#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "rtmp_server/control/http_server.hpp"
#include "rtmp_server/hls/playlist.hpp"
#include "rtmp_server/hls/segment_store.hpp"

namespace rtmp_server::control {

// MIME types HLS delivery requires.
inline constexpr const char* kContentTypeM3u8 = "application/vnd.apple.mpegurl";
inline constexpr const char* kContentTypeMpegTs = "video/mp2t";

struct HlsHttpOptions {
    // URL prefix these routes live under, e.g. "/hls".
    std::string route_prefix = "/hls";

    // When set, every playlist and segment request must carry a valid
    // signed playback token (`?token=...&expires=...`), verified with the
    // same management::verify_token used by the RTMP play path — so HLS and
    // RTMP playback enforce identical authorisation.
    bool require_playback_token = false;
    std::string token_signing_secret;

    // Cache-Control for the live media playlist. A live playlist changes
    // every segment duration, so it must never be cached beyond that or
    // viewers stall on a stale window.
    std::string playlist_cache_control = "no-cache, max-age=0";
    // Segments are immutable once published and are named uniquely, so they
    // are safely cacheable for a long time by any CDN.
    std::string segment_cache_control = "public, max-age=31536000, immutable";
    // Master playlists change only when the rendition set changes.
    std::string master_cache_control = "public, max-age=60";

    // Segment byte-range (HTTP Range) support. Players and CDNs use it for
    // partial fetches and resumed downloads.
    bool enable_range_requests = true;

    // Permissive CORS is required for browser players (hls.js) fetching
    // from a different origin. Empty disables the header.
    std::string cors_allow_origin = "*";
};

// Serves HLS playlists and segments over the existing Phase 5
// control::HttpServer — no second HTTP stack (docs/v2_promot.md Phase 6).
//
// Composed as a handler: install this in front of the ManagementApi handler
// and it forwards anything outside `route_prefix` to the next handler, so
// one HttpServer (with its Phase 5 bounded-connection posture: bounded
// accept queue, bounded header/body sizes, fixed worker pool) serves both.
//
// Routes, all GET/HEAD:
//   {prefix}/{application}/{stream}/master.m3u8   master (multivariant) playlist
//   {prefix}/{application}/{stream}/index.m3u8    media playlist
//   {prefix}/{application}/{stream}/{name}.ts     one segment
//
// Runs on HttpServer worker threads only — never on an RTMP media thread —
// and performs no disk I/O at all (segments are served from the in-memory
// SegmentStore).
class HlsHttpHandler {
public:
    explicit HlsHttpHandler(HlsHttpOptions options);

    // Registers/unregisters a stream's segment store. The store is owned by
    // the caller's stream lifecycle; a shared_ptr keeps it alive for the
    // duration of any in-flight request.
    void register_stream(const std::string& application, const std::string& stream,
                         std::shared_ptr<hls::SegmentStore> store);
    void unregister_stream(const std::string& application, const std::string& stream);
    [[nodiscard]] std::shared_ptr<hls::SegmentStore> find_stream(const std::string& application,
                                                                 const std::string& stream) const;

    // Declares the renditions advertised by master.m3u8 for a stream. With
    // a single passthrough rendition this is the ingest quality; adaptive
    // variants would be added here by a future transcoding worker.
    void set_renditions(const std::string& application, const std::string& stream,
                        std::vector<hls::Rendition> renditions);

    // Handler entry point. Suitable for HttpServer::set_handler directly, or
    // chained: set_next() is invoked for any path outside the prefix.
    [[nodiscard]] HttpResponse handle(const HttpRequest& request);
    void set_next(HttpHandler next) { next_ = std::move(next); }

    // Predicate consulted on every request: when it returns false for a
    // stream, playlists and segments 404 as if the stream did not exist. This
    // is how disabling a stream in the management API takes its HLS (.m3u8)
    // links offline, matching the RTMP publish/play gate. Unset = always served.
    using StreamEnabledChecker =
        std::function<bool(const std::string& application, const std::string& stream)>;
    void set_stream_enabled_checker(StreamEnabledChecker checker) {
        enabled_checker_ = std::move(checker);
    }

    struct Stats {
        std::uint64_t playlist_requests = 0;
        std::uint64_t segment_requests = 0;
        std::uint64_t unauthorized = 0;
        std::uint64_t not_found = 0;
        std::uint64_t range_requests = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct StreamEntry {
        std::shared_ptr<hls::SegmentStore> store;
        std::vector<hls::Rendition> renditions;
    };

    [[nodiscard]] bool authorized(const HttpRequest& request, const std::string& application,
                                  const std::string& stream) const;
    [[nodiscard]] HttpResponse serve_media_playlist(const HttpRequest& request, const StreamEntry& entry,
                                                    const std::string& application,
                                                    const std::string& stream);
    [[nodiscard]] HttpResponse serve_master_playlist(const StreamEntry& entry);
    [[nodiscard]] HttpResponse serve_segment(const HttpRequest& request, const StreamEntry& entry,
                                             const std::string& name);
    void decorate(HttpResponse& response, const std::string& cache_control) const;

    HlsHttpOptions options_;
    HttpHandler next_;
    StreamEnabledChecker enabled_checker_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StreamEntry> streams_; // key: "app/stream"
    mutable Stats stats_;
};

} // namespace rtmp_server::control
