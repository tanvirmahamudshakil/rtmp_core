#pragma once

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rtmp_server/control/http_server.hpp"
#include "rtmp_server/dash/mpd.hpp"
#include "rtmp_server/dash/segment_store.hpp"

namespace rtmp_server::control {

inline constexpr const char* kContentTypeMpd = "application/dash+xml";
inline constexpr const char* kContentTypeMp4 = "video/mp4";

struct DashHttpOptions {
    std::string route_prefix = "/dash";

    std::string playlist_cache_control = "no-cache, max-age=0"; // manifest.mpd
    std::string segment_cache_control = "public, max-age=31536000, immutable";
    std::string init_cache_control = "public, max-age=31536000, immutable";

    bool enable_range_requests = true;
    std::string cors_allow_origin = "*";

    // Same multi-node edge / origin-shield gate as HlsHttpOptions, and the
    // same header, so one edge token protects both delivery surfaces.
    std::string edge_fetch_secret;
    std::string edge_fetch_header = "x-edge-token";

    // Advertised MinimumUpdatePeriod / SuggestedPresentationDelay /
    // TimeShiftBufferDepth. 0 lets build_mpd's own defaults apply.
    double minimum_update_period_seconds = 0.0;
    double suggested_presentation_delay_seconds = 0.0;
};

// Serves DASH manifests and fMP4 segments over the existing Phase 5
// control::HttpServer/AsyncHttpServer -- the DASH counterpart of
// HlsHttpHandler, same composition contract (chains to set_next() for
// anything outside route_prefix).
//
// Routes, all GET/HEAD:
//   {prefix}/{application}/{stream}/manifest.mpd
//   {prefix}/{application}/{stream}/{rep}/init.mp4
//   {prefix}/{application}/{stream}/{rep}/{name}.m4s
class DashHttpHandler {
public:
    explicit DashHttpHandler(DashHttpOptions options) : options_(std::move(options)) {}

    // One representation's store, registered under its own id. A stream
    // with one rendition (the common case) registers one representation
    // whose id matches the stream's own name.
    void register_representation(const std::string& application, const std::string& stream,
                                 const std::string& representation_id,
                                 std::shared_ptr<dash::SegmentStore> store);
    void unregister_stream(const std::string& application, const std::string& stream);

    // Declares the manifest metadata (bandwidth, codecs, geometry) for every
    // representation of a stream. Must be called at least once before
    // manifest.mpd can be served meaningfully; an empty declaration serves a
    // manifest with no representations, matching HlsHttpHandler's
    // set_renditions/master.m3u8 behaviour for an undeclared stream.
    void set_representations(const std::string& application, const std::string& stream,
                             std::vector<dash::Representation> representations,
                             std::uint32_t timescale = 90000,
                             std::uint64_t segment_duration = 0);

    [[nodiscard]] HttpResponse handle(const HttpRequest& request);
    void set_next(HttpHandler next) { next_ = std::move(next); }

    using StreamEnabledChecker =
        std::function<bool(const std::string& application, const std::string& stream)>;
    void set_stream_enabled_checker(StreamEnabledChecker checker) {
        enabled_checker_ = std::move(checker);
    }

    struct Stats {
        std::uint64_t manifest_requests = 0;
        std::uint64_t segment_requests = 0;
        std::uint64_t not_found = 0;
        std::uint64_t edge_unauthorized = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct RepresentationEntry {
        std::shared_ptr<dash::SegmentStore> store;
        dash::Representation descriptor;
    };

    struct StreamEntry {
        std::shared_mutex mutex; // guards representations
        std::unordered_map<std::string, RepresentationEntry> representations;
        std::atomic<std::uint32_t> timescale{90000};
        std::atomic<std::uint64_t> segment_duration{0};
    };
    using StreamEntryPtr = std::shared_ptr<StreamEntry>;

    [[nodiscard]] StreamEntryPtr find_entry(const std::string& application,
                                            const std::string& stream) const;
    [[nodiscard]] StreamEntryPtr find_or_create_entry(const std::string& application,
                                                      const std::string& stream);

    [[nodiscard]] HttpResponse serve_manifest(const StreamEntryPtr& entry);
    [[nodiscard]] HttpResponse serve_init(dash::SegmentStore& store);
    [[nodiscard]] HttpResponse serve_segment(const HttpRequest& request, dash::SegmentStore& store,
                                             const std::string& name);
    void decorate(HttpResponse& response, const std::string& cache_control) const;

    DashHttpOptions options_;
    HttpHandler next_;
    StreamEnabledChecker enabled_checker_;

    mutable std::shared_mutex streams_mutex_;
    std::unordered_map<std::string, StreamEntryPtr> streams_; // key: "app/stream"

    struct AtomicStats {
        std::atomic<std::uint64_t> manifest_requests{0};
        std::atomic<std::uint64_t> segment_requests{0};
        std::atomic<std::uint64_t> not_found{0};
        std::atomic<std::uint64_t> edge_unauthorized{0};
    };
    mutable AtomicStats stats_;
};

} // namespace rtmp_server::control
