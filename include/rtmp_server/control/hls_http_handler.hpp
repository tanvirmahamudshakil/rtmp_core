#pragma once

#include <chrono>
#include <cstdint>
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

    // Give every fresh playlist open a distinct, opaque playback session.
    // It normally travels in playlist/segment query strings. Shared-playlist
    // mode keeps it on the recurring playlist URL instead, still allowing the
    // edge to count two VLC instances behind the same NAT separately.
    bool enable_playback_sessions = false;
    // Injectable only so tests can make redirects deterministic. Empty uses
    // core::generate_secure_token(16), i.e. 128 bits from OpenSSL RAND_bytes.
    std::function<std::string()> playback_session_id_factory;

    // Keep the per-player redirect/session identity, but make the resulting
    // playlist body safe for a shared reverse-cache object. The redirect adds
    // `viewer_cache=1`; the production VCL only caches playlist requests that
    // carry that marker. Child playlist/segment URIs are deliberately left
    // undecorated, so one viewer's session can never leak through a cached
    // response body. The edge still counts recurring playlist requests by
    // their session-bearing public URL.
    bool enable_shared_playlist_cache = false;

    // Delivery accounting takes the handler's shared registry mutex on every
    // cache miss. Disable it when a shared reverse cache is the viewer-facing
    // delivery tier; edge/cache metrics should be used at that scale instead.
    bool track_delivery_stats = true;

    // Token/session query values normally have to be copied into child
    // playlist URIs. Shared-playlist mode overrides this to false because a
    // cached body must contain no viewer-specific state.
    bool propagate_query_to_playlist_uris = true;

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

    // Persistent-viewer-identity cookie. The playback-session-minting block
    // (see hls_http_handler.cpp) uses this to recognise a *returning* viewer
    // on a request that arrives with no viewer_session query at all — which,
    // in shared-playlist-cache mode, is every request that follows an
    // undecorated rendition link out of the cached master body. Without it,
    // each such hop mints a brand-new random session and the viewer count
    // becomes inflated/incoherent (see docs on the 2143be4 regression).
    //
    // Deliberately never consulted for the *cached* response bodies
    // themselves (media playlists/segments), only for the private,
    // never-cached 302 redirect that assigns a session — so it cannot leak
    // one viewer's identity into another viewer's shared cache entry. The
    // production VCL (deploy/varnish/streamforge.vcl) only strips
    // `Cookie` on requests it is about to hash into the shared cache; a bare
    // `.m3u8` request without `viewer_cache=1` takes the `return (pass)`
    // branch first and reaches the origin with its Cookie header intact.
    std::string viewer_cookie_name = "rtmp_viewer_id";
    // Long enough to survive a normal viewing session (including short
    // pauses/reconnects), short enough not to function as a long-term
    // tracking identifier.
    std::chrono::seconds viewer_cookie_max_age{24 * 60 * 60};
    // Set true when this handler's responses only ever reach viewers over
    // TLS (directly or via a proxy that only forwards HTTPS to it) so the
    // cookie can carry `Secure`. Left false by default because this handler
    // can be run in plain-HTTP deployments (docs/tls.md), where a `Secure`
    // cookie would simply never be sent back and defeat its own purpose.
    bool viewer_cookie_secure = false;
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

    // Live per-link delivery stats derived from actual HLS playlist/segment
    // requests — the only signal available for a source-transcode job's
    // rendition outputs, which never pass through LiveFanout (no RTMP
    // subscriber exists for a pure HLS puller/consumer). See docs/hls.md.
    struct LinkStats {
        std::uint64_t bytes_total = 0;
        // Distinct playback sessions that fetched a media playlist or segment
        // for this exact stream key within the last kViewerWindow. A viewer
        // idle longer than the window drops off promptly.
        std::size_t viewer_count = 0;
    };
    // Stats for exactly one registered stream key (one rendition, or a plain
    // stream's own HLS delivery).
    [[nodiscard]] LinkStats link_stats(const std::string& application, const std::string& stream);
    // Stats for a master/job name, summed across every rendition it
    // advertises (parsed from each Rendition::uri, always built as
    // "../" + output_stream + "/index.m3u8" by every renditions-ready
    // callback — see apps/rtmp_server/main.cpp and
    // src/transcoding/native/source_job_manager.cpp). Viewer sessions are
    // unioned across renditions, so an ABR switch never double-counts one
    // player. Falls back to the entry's own stats when it advertises no
    // renditions (a plain, non-ABR stream).
    [[nodiscard]] LinkStats aggregate_link_stats(const std::string& application, const std::string& master_stream);

private:
    // A client is still considered "watching" if it fetched a media playlist
    // or segment within this window. Set well above a typical segment
    // duration (2-8s here, see SegmenterConfig) so normal playlist refresh
    // cadence never drops a still-connected viewer, but short enough that a
    // closed player disappears from the count promptly.
    static constexpr std::chrono::seconds kViewerWindow{20};
    // Hard cap on tracked sessions per stream key, swept opportunistically once
    // exceeded. Bounds memory even if a stream is hit by a huge number of
    // distinct, non-returning sessions (e.g. an unauthenticated scrape).
    static constexpr std::size_t kMaxTrackedViewerSessions = 20000;

    struct StreamEntry {
        std::shared_ptr<hls::SegmentStore> store;
        std::vector<hls::Rendition> renditions;
        // Both guarded by mutex_, same lock already taken per-request for
        // stats_ below — piggybacking avoids adding new contention.
        std::uint64_t bytes_total = 0;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> recent_viewer_sessions;
    };

    // Records one successful delivery for `application/stream` — called for
    // every 2xx/206 media-playlist or segment response. No-op if the stream
    // was unregistered between building the response and this call.
    void record_delivery(const std::string& application, const std::string& stream, std::uint64_t bytes,
                         const std::string& playback_session);

    [[nodiscard]] bool authorized(const HttpRequest& request, const std::string& application,
                                  const std::string& stream) const;
    [[nodiscard]] HttpResponse serve_media_playlist(const HttpRequest& request, const StreamEntry& entry,
                                                    const std::string& application,
                                                    const std::string& stream);
    [[nodiscard]] HttpResponse serve_master_playlist(const HttpRequest& request, const StreamEntry& entry);
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
