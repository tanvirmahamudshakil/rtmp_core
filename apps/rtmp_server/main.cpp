#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rtmp_server/authentication/rtmp_authenticator.hpp"
#include "rtmp_server/cluster/node_registry.hpp"
#include "rtmp_server/dash/stream_sink.hpp"
#include "rtmp_server/control/edge_viewer_stats.hpp"
#include "rtmp_server/control/async_http_server.hpp"
#include "rtmp_server/control/dash_http_handler.hpp"
#include "rtmp_server/control/hls_http_handler.hpp"
#include "rtmp_server/control/http_server.hpp"
#include "rtmp_server/control/management_api.hpp"
#include "rtmp_server/control/settings_codec.hpp"
#include "rtmp_server/core/config.hpp"
#include "rtmp_server/core/error.hpp"
#include "rtmp_server/io/io_uring/worker_pool.hpp"
#include "rtmp_server/hls/stream_sink.hpp"
#include "rtmp_server/management/stream_manager.hpp"
#include "rtmp_server/observability/audit_log.hpp"
#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/persistence/sqlite_store.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"
#include "rtmp_server/relay/backup_publisher.hpp"
#include "rtmp_server/relay/stream_target.hpp"
#include "rtmp_server/relay/stream_target_manager.hpp"
#include "rtmp_server/transcoding/preset.hpp"
#ifdef RTMP_NATIVE_TRANSCODE
#include "rtmp_server/transcoding/native/ingest_transcoder.hpp"
#include "rtmp_server/transcoding/native/source_job_manager.hpp"
#endif

namespace {

rtmp_server::io::io_uring::WorkerPool* g_pool = nullptr;

// Signal handlers must be async-signal-safe: only touch an atomic/flag and
// return. Actual teardown happens on each worker's own thread inside its
// run(), per docs/rtmp_promot.md "Graceful Shutdown".
void handle_shutdown_signal(int /*signum*/) {
    if (g_pool != nullptr) g_pool->stop();
}

void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = handle_shutdown_signal;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    // Prevent SIGPIPE from killing the process on writes to a peer that
    // already reset the connection (docs/rtmp_promot.md "io_uring Error
    // Classification").
    ::signal(SIGPIPE, SIG_IGN);
}

// Fans one publisher's media out to several packaging sinks. The publish path
// hands a stream exactly one RecorderSink, and a stream can now need more than
// one: passthrough HLS, optionally DASH, and -- when a transcoding assignment
// covers it -- a re-encoded rendition ladder.
class FanOutSink final : public rtmp_server::protocol::commands::RecorderSink {
public:
    using Sink = rtmp_server::protocol::commands::RecorderSink;

    explicit FanOutSink(std::vector<std::shared_ptr<Sink>> sinks) : sinks_(std::move(sinks)) {}

    void on_metadata(const rtmp_server::protocol::chunk::RtmpMessage& message) override {
        for (auto& sink : sinks_) sink->on_metadata(message);
    }
    void on_audio(const rtmp_server::protocol::chunk::RtmpMessage& message) override {
        for (auto& sink : sinks_) sink->on_audio(message);
    }
    void on_video(const rtmp_server::protocol::chunk::RtmpMessage& message) override {
        for (auto& sink : sinks_) sink->on_video(message);
    }
    void finalize() override {
        for (auto& sink : sinks_) sink->finalize();
    }

private:
    std::vector<std::shared_ptr<Sink>> sinks_;
};

// Cluster identity comes from the environment rather than server.yaml: the
// installer already writes systemd environment for a node, edges are not
// running this binary at all (they heartbeat from a shell timer, see
// deploy/edge/), and an id that differs per machine does not belong in a file
// that is copied between them.
std::string environment_or(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    return std::string(value);
}

std::string local_hostname() {
    std::array<char, 256> buffer{};
    if (::gethostname(buffer.data(), buffer.size() - 1) != 0) return "origin";
    return std::string(buffer.data());
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(c); break;
        }
    }
    return result;
}

std::string cluster_node_json(const rtmp_server::cluster::NodeStatus& node) {
    std::ostringstream os;
    os << R"({"id":")" << json_escape(node.id) << R"(","role":")"
       << json_escape(rtmp_server::cluster::to_string(node.role)) << R"(","address":")"
       << json_escape(node.address) << R"(","region":")" << json_escape(node.region)
       << R"(","healthy":)" << (node.healthy ? "true" : "false") << R"(,"draining":)"
       << (node.draining ? "true" : "false") << R"(,"seconds_since_seen":)"
       << node.seconds_since_seen << R"(,"capacity_viewers":)" << node.capacity_viewers
       << R"(,"active_viewers":)" << node.active_viewers << R"(,"active_publishers":)"
       << node.active_publishers << R"(,"load":)" << node.load << '}';
    return os.str();
}

std::string stream_target_json(const rtmp_server::relay::StreamTargetStatus& target) {
    const auto state = [&] {
        switch (target.state) {
            case rtmp_server::relay::StreamTargetState::Connecting: return "connecting";
            case rtmp_server::relay::StreamTargetState::Publishing: return "publishing";
            case rtmp_server::relay::StreamTargetState::Error: return "error";
            case rtmp_server::relay::StreamTargetState::Stopped: return "stopped";
        }
        return "stopped";
    }();
    std::ostringstream os;
    // `url` is deliberately the redacted form: the stored URL ends in the
    // destination's stream key, which this API must never hand back out.
    os << R"({"id":")" << json_escape(target.application + ":" + target.stream + ":" + target.name)
       << R"(","application":")" << json_escape(target.application) << R"(","stream":")"
       << json_escape(target.stream) << R"(","name":")" << json_escape(target.name)
       << R"(","url":")" << json_escape(target.url_redacted) << R"(","relay":)"
       << (target.relay ? "true" : "false") << R"(,"enabled":)"
       << (target.enabled ? "true" : "false") << R"(,"state":")" << state << R"(","detail":")"
       << json_escape(target.detail) << R"(","bytes_sent":)" << target.bytes_sent
       << R"(,"frames_dropped":)" << target.frames_dropped << R"(,"reconnects":)"
       << target.reconnects << '}';
    return os.str();
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "./config/server.yaml";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    auto config_result = rtmp_server::core::load_config(config_path);
    if (!config_result) {
        std::fprintf(stderr, "failed to load config '%s': %s\n", config_path.c_str(),
                      config_result.error().message().c_str());
        return EXIT_FAILURE;
    }

    using rtmp_server::observability::LogLevel;
    auto& logger = rtmp_server::observability::Logger::instance();
    if (config_result.value().log_level == "debug") logger.set_level(LogLevel::Debug);
    else if (config_result.value().log_level == "trace") logger.set_level(LogLevel::Trace);
    else if (config_result.value().log_level == "warn") logger.set_level(LogLevel::Warn);
    else if (config_result.value().log_level == "error") logger.set_level(LogLevel::Error);

    const auto& config = config_result.value();

    if (config.database_type != "sqlite") {
        std::fprintf(stderr, "unsupported database_type '%s' (this single-node build requires sqlite)\n",
                     config.database_type.c_str());
        return EXIT_FAILURE;
    }

    auto store_result = rtmp_server::persistence::SqliteStore::open(config.database_connection);
    if (!store_result) {
        std::fprintf(stderr, "failed to open database '%s': %s\n", config.database_connection.c_str(),
                     store_result.error().message().c_str());
        return EXIT_FAILURE;
    }
    auto store = std::move(store_result).value();

    rtmp_server::observability::Metrics metrics;
    rtmp_server::observability::AuditLog audit_log;
    rtmp_server::management::StreamManager stream_manager(
        {config.public_rtmp_hostname, config.rtmp_port, config.token_signing_secret});
    stream_manager.set_store(store.get());
    stream_manager.set_audit_log(&audit_log);
    stream_manager.set_metrics(&metrics);
    auto load_result = stream_manager.load_from_store();
    if (!load_result) {
        std::fprintf(stderr, "failed to load persisted stream configuration: %s\n",
                     load_result.error().message().c_str());
        return EXIT_FAILURE;
    }

    rtmp_server::authentication::AuthenticatorLimits auth_limits;
    auth_limits.max_publishers_per_stream = 1;
    auth_limits.max_viewers_per_stream = config.maximum_viewers_per_stream;
    auth_limits.max_connections_per_ip = config.maximum_connections_per_ip;
    rtmp_server::authentication::RtmpAuthenticator authenticator(stream_manager, auth_limits);
    authenticator.set_metrics(&metrics);

    // Process-wide RTMP stream identity tables (Phase 1), shared by
    // reference across every WorkerPool worker so a publish/playback name
    // resolves to the same StreamId regardless of which worker's ring
    // accepted the connection (docs/v2_promot.md PHASE 4 task "avoid
    // cross-worker access to mutable connection state" — these two are the
    // one deliberate, mutex-guarded exception, by design; see
    // IoUringEventLoop's class doc).
    rtmp_server::protocol::commands::StreamRegistry stream_registry;
    rtmp_server::protocol::commands::StreamIdRegistry stream_id_registry;
    stream_registry.set_metrics(&metrics);

    rtmp_server::io::io_uring::EventLoopServices services;
    services.metrics = &metrics;
    // Open publishing: OBS/ffmpeg publishes with the public stream name,
    // not a secret credential. Requiring an existing enabled application and
    // stream preserves deterministic routing and the operator's disable
    // control without adding authentication.
    services.key_validator =
        [&stream_manager](std::string_view application, std::string_view stream_name) {
            const auto app = stream_manager.find_application(application);
            const auto stream = stream_manager.find_stream(application, stream_name);
            return app && app->enabled && stream && stream->enabled;
        };
    // The publish argument is already the canonical public fan-out name.
    services.stream_id_resolver = {};
    services.playback_authorizer = authenticator.playback_authorizer();

    // Per-publisher C++ HLS pipeline: H.264/AAC is repackaged (never
    // transcoded) into bounded, immutable MPEG-TS segments. Each HTTP viewer
    // shares the same segment bytes instead of allocating its own media copy.
    rtmp_server::control::HlsHttpOptions hls_options;
    hls_options.require_playback_token = false;
    hls_options.enable_playback_sessions = true;
    // The shared Varnish cache in front of this origin (deploy/varnish/
    // streamforge.vcl) is the viewer-facing delivery tier. Two things have to
    // line up for a media playlist to be collapsed there instead of reaching
    // the origin once per viewer per segment duration:
    //   * the redirect must carry `viewer_cache=1`, which is the only marker
    //     vcl_recv accepts before hashing an .m3u8 rather than passing it, and
    //   * the response must advertise a `public` Cache-Control, which is what
    //     vcl_backend_response requires before giving the object a TTL.
    // Without both, every viewer's playlist poll became an origin request that
    // took the handler's shared registry mutex and the publisher's SegmentStore
    // mutex, so viewer count stole CPU from the encoders and pushed transcoded
    // output behind real time. Shared mode also forces playlist bodies to stay
    // free of per-viewer query state (see the HlsHttpHandler constructor), which
    // is what makes one cached object correct for every player.
    hls_options.enable_shared_playlist_cache = true;
    hls_options.playlist_cache_control = "public, max-age=1, s-maxage=1, stale-while-revalidate=2";
    // Delivery accounting stays on. The admin panel prefers the edge's own
    // per-session numbers from /internal/viewer_estimate.json when they are
    // fresh and match a stream's key (admin/src/api.ts sourceEdgeValue /
    // exactEdgeValue), but a source-transcode job's viewer_count falls back
    // to this handler's own aggregate_link_stats() whenever the edge file is
    // stale, unreachable, or its rendition keys don't match yet — turning
    // this off left that fallback permanently reporting zero viewers instead
    // of only when the origin is otherwise idle. Cache collapsing above
    // already cut playlist volume at the origin to ~1 req/s per stream, so
    // this mutex is no longer the contention source it was before.
    hls_options.track_delivery_stats = true;
    hls_options.enable_fast_join = config.enable_hls_fast_join;
    // Multi-node edge / origin-shield gate. Empty by default (single-box
    // install: only the co-located Varnish reaches this handler). When set,
    // the origin serves /hls only to a request carrying X-Edge-Token, so it
    // cannot be hit directly or used as an open segment proxy once an edge
    // tier (deploy/varnish/streamforge-edge.vcl, deploy/edge/install-edge.sh)
    // is in front. See docs/multi-node-hls.md.
    hls_options.edge_fetch_secret = config.hls_edge_fetch_secret;
    // Low-Latency HLS and trick play. Both are additive: a player that knows
    // neither tag sees the same playlist it always did.
    hls_options.enable_low_latency = config.hls_low_latency;
    hls_options.blocking_reload_timeout = config.hls_blocking_reload_timeout;
    hls_options.enable_iframe_playlists = config.hls_iframe_playlists;
    rtmp_server::control::HlsHttpHandler hls_handler(std::move(hls_options));
    // Disabling a stream (or its application) takes its .m3u8 links offline
    // immediately, mirroring the RTMP publish/play gate above — no viewer can
    // keep pulling HLS from a stream the operator has disabled.
    hls_handler.set_stream_enabled_checker(
        [&stream_manager](const std::string& application, const std::string& stream) {
            // Block a stream (or its application) only when it exists and is
            // explicitly disabled. Streams registered purely for HLS serving —
            // e.g. source-transcode rendition outputs, which have no managed
            // stream record — are served normally.
            const auto app = stream_manager.find_application(application);
            if (app && !app->enabled) return false;
            const auto meta = stream_manager.find_stream(application, stream);
            if (meta && !meta->enabled) return false;
            return true;
        });

    // MPEG-DASH delivery, off the same publish as HLS. Off by default
    // (config.dash_enabled); when off, dash_handler is still constructed
    // (cheap -- no representations are ever registered) but chained after
    // hls_handler regardless, so enabling it later needs no code change,
    // only a config flag and a restart.
    rtmp_server::control::DashHttpOptions dash_options;
    dash_options.edge_fetch_secret = config.hls_edge_fetch_secret;
    rtmp_server::control::DashHttpHandler dash_handler(std::move(dash_options));
    dash_handler.set_stream_enabled_checker(
        [&stream_manager](const std::string& application, const std::string& stream) {
            const auto app = stream_manager.find_application(application);
            if (app && !app->enabled) return false;
            const auto meta = stream_manager.find_stream(application, stream);
            if (meta && !meta->enabled) return false;
            return true;
        });

    // Cluster membership. Edges, shields and transcoder nodes run no database
    // of their own, so they announce themselves to this origin by heartbeat;
    // the table is what /v1/cluster/locate places viewers from.
    rtmp_server::cluster::NodeRegistry node_registry(store.get(), {});
    node_registry.load_from_store();

    // Outbound RTMP: pushing this server's publishes to another origin (the
    // relay/repeater, which is how ingest scales past this box) or to an
    // external ingest (stream targets).
    rtmp_server::relay::StreamTargetManager stream_targets(store.get(), {});
    stream_targets.load_from_store();

#ifdef RTMP_NATIVE_TRANSCODE
    // Transcoding of streams published *to* this origin. A stream with a
    // transcoding assignment keeps its untranscoded /hls/<app>/<stream>/
    // index.m3u8 exactly as before and additionally gets a re-encoded ladder,
    // one HLS stream per rung, advertised together from
    // /hls/<app>/<stream>/master.m3u8. Streams without an assignment -- the
    // common case -- are untouched: create_sink returns nullptr and the
    // publish path is byte-for-byte the passthrough one.
    rtmp_server::transcoding::native::IngestTranscodeManager::Hooks ingest_hooks;
    ingest_hooks.set_renditions = [&hls_handler](const std::string& application,
                                                 const std::string& master,
                                                 std::vector<rtmp_server::hls::Rendition> renditions) {
        hls_handler.set_renditions(application, master, std::move(renditions));
    };
    ingest_hooks.register_output = [&hls_handler](const std::string& application,
                                                  const std::string& stream,
                                                  std::shared_ptr<rtmp_server::hls::SegmentStore> store) {
        hls_handler.register_stream(application, stream, std::move(store));
    };
    ingest_hooks.unregister_output = [&hls_handler](const std::string& application,
                                                    const std::string& stream) {
        hls_handler.unregister_stream(application, stream);
    };
    rtmp_server::transcoding::native::IngestTranscodeOptions ingest_options;
    // Same segment shape as the passthrough sink below, so both surfaces of
    // one publish advertise the same window depth and target duration.
    ingest_options.target_duration_seconds = 6;
    ingest_options.live_window_segments = 10;
    ingest_options.retention_grace_segments = 6;
    ingest_options.max_total_bytes_per_rendition = 256u * 1024u * 1024u;
    ingest_options.segment_target_duration = std::chrono::seconds(6);
    ingest_options.max_segment_duration = std::chrono::seconds(12);
    if (config.hls_low_latency) {
        ingest_options.part_target_duration = config.hls_part_target_duration;
    }
    ingest_options.transcode_cpu_reservation_percent = config.transcode_cpu_reservation_percent;
    rtmp_server::transcoding::native::IngestTranscodeManager ingest_transcoder(
        std::move(ingest_hooks), store.get(), ingest_options);
    ingest_transcoder.load_from_store();
#endif

    services.recorder_factory =
        [&hls_handler, &dash_handler, &config, &stream_targets
#ifdef RTMP_NATIVE_TRANSCODE
         , &ingest_transcoder
#endif
        ](std::string_view application,
                       std::string_view stream) -> std::shared_ptr<rtmp_server::protocol::commands::RecorderSink> {
            rtmp_server::hls::SegmentStoreConfig store_config;
            // A deeper live window (10 x 6 s = 60 s) gives players a large
            // rebuffer cushion, so an occasional upstream hiccup or backend
            // hop does not stall playback.
            store_config.live_window_segments = 10;
            store_config.retention_grace_segments = 6;
            store_config.max_total_bytes = 256u * 1024u * 1024u;
            // 6 s segments: at a large single-box audience this cuts each
            // viewer's playlist+segment request rate to a third of what 2 s
            // segments produce (fewer packets, connections and conntrack
            // churn) and keeps every fetch in bulk TCP transfer rather than
            // slow-start. Trade-off is ~12-18 s more glass-to-glass latency,
            // which a rebroadcast/IPTV audience does not notice. The encoder
            // keyframe interval must divide this (2 s or 3 s GOP).
            store_config.target_duration_seconds = 6;
            store_config.low_latency = config.hls_low_latency;
            store_config.part_target_duration = config.hls_part_target_duration;
            auto store = std::make_shared<rtmp_server::hls::SegmentStore>(store_config);

            if (config.hls_encryption_enabled) {
                rtmp_server::hls::EncryptionConfig encryption;
                encryption.enabled = true;
                encryption.rotation_interval = config.hls_key_rotation_interval;
                // The key sits beside the playlist, so a player resolves it
                // relative to the same stream path and it passes through the
                // same playback authorisation as the media it decrypts.
                encryption.key_uri_template = "key-{kid}.bin";
                auto encryptor =
                    std::make_shared<rtmp_server::hls::SegmentEncryptor>(std::move(encryption));
                store->set_encryptor(encryptor);
                hls_handler.set_encryptor(std::string(application), std::string(stream),
                                          std::move(encryptor));
            }

            rtmp_server::hls::SegmenterConfig segmenter_config;
            segmenter_config.target_duration = std::chrono::seconds(6);
            if (config.hls_low_latency) {
                segmenter_config.part_target_duration = config.hls_part_target_duration;
            }
            // Headroom above target so a stream whose keyframe cadence is not
            // a clean divisor of 6 s is still cut on a keyframe rather than
            // force-split mid-GOP.
            segmenter_config.max_segment_duration = std::chrono::seconds(12);
            segmenter_config.max_segment_bytes = 24u * 1024u * 1024u;

            hls_handler.register_stream(std::string(application), std::string(stream), store);
            auto hls_sink = std::make_shared<rtmp_server::hls::StreamSink>(std::move(store),
                                                                          std::move(segmenter_config));

            // The transcoded ladder, when an assignment covers this stream.
            // Null for every unassigned publish, which is the common case and
            // keeps that path exactly as it was.
            std::shared_ptr<rtmp_server::protocol::commands::RecorderSink> transcode_sink;
#ifdef RTMP_NATIVE_TRANSCODE
            transcode_sink = ingest_transcoder.create_sink(application, stream);
#endif
            // Outbound pushes (relay origins, CDN/social ingests), likewise
            // null unless this stream has enabled targets.
            auto target_sink = stream_targets.create_sink(application, stream);

            std::vector<std::shared_ptr<rtmp_server::protocol::commands::RecorderSink>> extra;
            if (transcode_sink) extra.push_back(std::move(transcode_sink));
            if (target_sink) extra.push_back(std::move(target_sink));

            if (!config.dash_enabled) {
                if (extra.empty()) return hls_sink;
                extra.insert(extra.begin(), std::move(hls_sink));
                return std::make_shared<FanOutSink>(std::move(extra));
            }

            // Same 6 s cadence as the HLS store, so both delivery surfaces
            // advertise the same live window depth for one publisher.
            rtmp_server::dash::SegmentStoreConfig dash_store_config;
            dash_store_config.live_window_segments = 10;
            dash_store_config.retention_grace_segments = 6;
            dash_store_config.max_total_bytes = 256u * 1024u * 1024u;
            dash_store_config.target_duration_seconds = 6;
            auto dash_store = std::make_shared<rtmp_server::dash::SegmentStore>(dash_store_config);

            rtmp_server::dash::SegmenterConfig dash_segmenter_config;
            dash_segmenter_config.target_duration = std::chrono::seconds(6);
            dash_segmenter_config.max_segment_duration = std::chrono::seconds(12);
            dash_segmenter_config.max_segment_bytes = 24u * 1024u * 1024u;
            auto dash_sink = std::make_shared<rtmp_server::dash::StreamSink>(
                dash_store, std::move(dash_segmenter_config));

            const std::string app_str(application);
            const std::string stream_str(stream);
            // The representation id doubles as the DASH URL path component
            // ({prefix}/{app}/{stream}/{rep}/...); reusing the stream name
            // itself keeps a single-rendition publish's DASH URL as
            // predictable as its HLS one.
            dash_handler.register_representation(app_str, stream_str, stream_str, dash_store);

            // The manifest needs bandwidth/codecs/geometry up front, but
            // those are only known once the first sequence headers arrive.
            // Poll for them lazily from a background-cheap lambda invoked on
            // the first manifest request that finds no representations
            // declared yet would be more invasive than this: instead,
            // publish_start_handler_ below (re-)declares the representation
            // once the segmenter has both configs, which happens within the
            // first GOP -- well before any player's first manifest fetch in
            // practice, and a fetch that races it simply sees an empty
            // manifest once and a populated one on the player's retry.
            struct CombinedSink final : rtmp_server::protocol::commands::RecorderSink {
                std::shared_ptr<rtmp_server::hls::StreamSink> hls;
                std::shared_ptr<rtmp_server::dash::StreamSink> dash;
                rtmp_server::control::DashHttpHandler* handler;
                std::string application;
                std::string stream;
                std::string representation_id;
                bool declared = false;

                void maybe_declare() {
                    if (declared) return;
                    if (!dash->segmenter().has_video_config() || !dash->segmenter().has_audio_config()) {
                        return;
                    }
                    rtmp_server::dash::Representation rep;
                    rep.id = representation_id;
                    rep.codecs = dash->segmenter().codecs_attribute();
                    rep.mime_type = "video/mp4";
                    rep.init_template = "{rep}/init.mp4";
                    rep.media_template = "{rep}/chunk-$Number$.m4s";
                    // Passthrough ingest, so bitrate is whatever the
                    // publisher sends; a fixed planning figure keeps the MPD
                    // valid without measuring live throughput per rendition
                    // (matches the HLS master playlist, which has the same
                    // gap -- see docs/hls.md "Multiple renditions").
                    rep.bandwidth = 3'000'000;
                    std::vector<rtmp_server::dash::Representation> reps{std::move(rep)};
                    handler->set_representations(application, stream, std::move(reps),
                                                 rtmp_server::media::mp4::kVideoTimescale,
                                                 6 * rtmp_server::media::mp4::kVideoTimescale);
                    declared = true;
                }

                void on_metadata(const rtmp_server::protocol::chunk::RtmpMessage& message) override {
                    hls->on_metadata(message);
                    dash->on_metadata(message);
                    maybe_declare();
                }
                void on_audio(const rtmp_server::protocol::chunk::RtmpMessage& message) override {
                    hls->on_audio(message);
                    dash->on_audio(message);
                    maybe_declare();
                }
                void on_video(const rtmp_server::protocol::chunk::RtmpMessage& message) override {
                    hls->on_video(message);
                    dash->on_video(message);
                    maybe_declare();
                }
                void finalize() override {
                    hls->finalize();
                    dash->finalize();
                }
            };

            auto combined = std::make_shared<CombinedSink>();
            combined->hls = std::move(hls_sink);
            combined->dash = std::move(dash_sink);
            combined->handler = &dash_handler;
            combined->application = app_str;
            combined->stream = stream_str;
            combined->representation_id = stream_str;
            if (extra.empty()) return combined;
            extra.insert(extra.begin(), std::move(combined));
            return std::make_shared<FanOutSink>(std::move(extra));
        };

    // Backup publisher failover: when a stream's primary publisher has been
    // absent past its configured grace period, a designated backup RTMP
    // source is played and fed into the same packaging path a real publish
    // uses -- reusing recorder_factory itself (copied here, before it is
    // moved into WorkerPool below) means a failover produces the identical
    // HLS/DASH/transcode/target fan-out, with no separate code path to keep
    // in sync.
    const auto backup_publisher_sink_factory = services.recorder_factory;
    rtmp_server::relay::BackupPublisherManager backup_publishers(
        store.get(),
        [&stream_registry](std::string_view application, std::string_view stream) {
            const auto registrations = stream_registry.snapshot();
            return std::ranges::any_of(registrations, [&](const auto& registration) {
                return registration.app == application && registration.stream_key == stream;
            });
        },
        backup_publisher_sink_factory);
    backup_publishers.load_from_store();

    services.viewer_attached_handler =
        [&authenticator](std::string_view application, std::string_view stream_name) {
            authenticator.on_viewer_attached(application, stream_name);
        };
    services.viewer_detached_handler =
        [&authenticator](std::string_view application, std::string_view stream_name) {
            authenticator.on_viewer_detached(application, stream_name);
        };
    services.admit_connection = [&authenticator](std::string_view client_ip) {
        return authenticator.admit_connection(client_ip);
    };
    services.release_connection = [&authenticator](std::string_view client_ip) {
        authenticator.release_connection(client_ip);
    };
    rtmp_server::io::io_uring::WorkerPool pool(config, stream_registry, stream_id_registry, std::move(services));

    // Open control plane: the management API is served without a bearer-token
    // check so hitting the web panel drops the operator straight into the
    // dashboard. Anyone who can reach this endpoint can create, disable and
    // disconnect streams.
    rtmp_server::control::ManagementApiOptions management_options{config.api_authentication_secret};
    management_options.require_authentication = false;
    rtmp_server::control::ManagementApi management_api(stream_manager, management_options);
    management_api.set_store(store.get());
    management_api.set_audit_log(&audit_log);
    management_api.set_metrics(&metrics);
    management_api.set_stream_id_registry(&stream_id_registry);
    // Admin Settings page: reads/writes config_path directly. Every field
    // only takes effect on the next restart (see settings_codec.hpp) -- the
    // operator is expected to restart the service after saving, exactly as
    // editing the YAML file by hand and restarting always required.
    management_api.set_settings_handlers(
        [&config_path]() -> rtmp_server::core::Result<std::string> {
            return rtmp_server::control::settings_to_json(config_path);
        },
        [&config_path](const std::unordered_map<std::string, std::string>& updates)
            -> rtmp_server::core::Result<std::string> {
            return rtmp_server::control::apply_settings_updates(config_path, updates);
        });
    management_api.set_stream_deleted_handler(
        [&hls_handler, &dash_handler, &stream_targets
#ifdef RTMP_NATIVE_TRANSCODE
         , &ingest_transcoder
#endif
        ](std::string_view application, std::string_view stream) {
            hls_handler.unregister_stream(std::string(application), std::string(stream));
            dash_handler.unregister_stream(std::string(application), std::string(stream));
#ifdef RTMP_NATIVE_TRANSCODE
            // A deleted stream's transcoded rungs are derived from it and must
            // not outlive it; the assignment itself is left alone, so
            // recreating the stream brings its ladder back.
            ingest_transcoder.release(application, stream);
#endif
            stream_targets.release(application, stream);
        });
    // Real per-link viewer counting. The origin only ever sees the requests
    // Varnish could not answer from cache -- roughly one per second per link
    // no matter how large the audience -- so the cache edge's own accounting
    // (viewer-estimator.service) is the only place a true HLS viewer count
    // exists. Reading it here means every consumer of this server's API gets
    // the real number, instead of the admin panel being the one client that
    // knew to go and fetch that file for itself.
    rtmp_server::control::EdgeViewerStats::Options edge_viewer_options;
    edge_viewer_options.path = config.edge_viewer_stats_path;
    rtmp_server::control::EdgeViewerStats edge_viewers(edge_viewer_options);

    management_api.set_live_state_provider([&stream_manager, &stream_registry, &stream_id_registry,
                                            &pool, &hls_handler, &edge_viewers] {
        std::vector<rtmp_server::management::LiveState> states;
        const auto registrations = stream_registry.snapshot();
        const auto edge = edge_viewers.snapshot();
        for (const auto& application : stream_manager.list_applications()) {
            for (const auto& stream : stream_manager.list_streams(application.name)) {
                rtmp_server::management::LiveState state;
                state.application = stream.application;
                state.name = stream.name;
                state.is_live = std::ranges::any_of(registrations, [&stream](const auto& registration) {
                    return registration.app == stream.application && registration.stream_key == stream.name;
                });
                std::uint64_t rtmp_bytes = 0;
                if (state.is_live) {
                    if (auto id = stream_id_registry.find(stream.application, stream.name)) {
                        state.rtmp_viewer_count = pool.subscriber_count(*id);
                        rtmp_bytes = pool.egress_bytes_total(*id);
                    }
                }
                // A published stream's HLS link is /hls/<app>/<name>/index.m3u8,
                // so the edge keys its sessions under exactly "app/name".
                const auto link = state.application + "/" + state.name;
                state.hls_viewers_measured = edge.fresh;
                std::uint64_t hls_bytes = 0;
                if (edge.fresh) {
                    if (const auto it = edge.viewers.find(link); it != edge.viewers.end()) {
                        state.hls_viewer_count = static_cast<std::size_t>(it->second);
                    }
                    if (const auto it = edge.bytes_total.find(link); it != edge.bytes_total.end()) {
                        hls_bytes = it->second;
                    }
                } else {
                    // No edge reading: fall back to what this origin itself
                    // delivered. Behind a cache that is a floor, not a count,
                    // which hls_viewers_measured=false tells the caller.
                    const auto origin = hls_handler.link_stats(state.application, state.name);
                    state.hls_viewer_count = origin.viewer_count;
                    hls_bytes = origin.bytes_total;
                }
                state.viewer_count = state.rtmp_viewer_count + state.hls_viewer_count;
                // Everything this link has cost, both delivery paths together
                // — the panel derives its per-link bitrate from the delta.
                state.rtmp_egress_bytes_total = rtmp_bytes;
                state.hls_egress_bytes_total = hls_bytes;
                state.egress_bytes_total = rtmp_bytes + hls_bytes;
                states.push_back(std::move(state));
            }
        }
        return states;
    });

    // Publish the edge's deduplicated totals as gauges so /metrics carries
    // the same viewer figure the per-link numbers add up to. The panel used
    // to synthesise these client-side from the raw file.
    management_api.set_metrics_refresher([&metrics, &edge_viewers] {
        const auto edge = edge_viewers.snapshot();
        metrics.set_gauge("edge_delivery_stats_available", edge.fresh ? 1 : 0);
        if (!edge.fresh) return;
        metrics.set_gauge("hls_active_viewers", static_cast<std::int64_t>(edge.total_viewers));
        metrics.set_gauge("hls_egress_bytes_total", static_cast<std::int64_t>(edge.total_bytes));
        metrics.set_gauge("hls_egress_bitrate", static_cast<std::int64_t>(edge.total_bitrate_bps));
    });

    // Cluster: membership, heartbeats and viewer placement.
    management_api.set_cluster_handlers(
        [&node_registry]() -> rtmp_server::core::Result<std::string> {
            const auto now = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            std::ostringstream os;
            os << R"({"items":[)";
            const auto nodes = node_registry.list(now);
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (i) os << ',';
                os << cluster_node_json(nodes[i]);
            }
            os << "]}";
            return os.str();
        },
        [&node_registry](const std::unordered_map<std::string, std::string>& fields)
            -> rtmp_server::core::Result<std::string> {
            const auto field = [&fields](const char* key) -> std::string {
                const auto it = fields.find(key);
                return it == fields.end() ? std::string{} : it->second;
            };
            const auto number = [&field](const char* key) -> std::uint32_t {
                const auto text = field(key);
                if (text.empty()) return 0;
                errno = 0;
                const auto value = std::strtoull(text.c_str(), nullptr, 10);
                if (errno != 0 || value > std::numeric_limits<std::uint32_t>::max()) return 0;
                return static_cast<std::uint32_t>(value);
            };
            const auto role = rtmp_server::cluster::parse_node_role(field("role"));
            if (!role) {
                return rtmp_server::core::Error(
                    rtmp_server::core::ErrorCode::InvalidConfiguration,
                    rtmp_server::core::ErrorCategory::Configuration,
                    "role must be origin, edge, shield or transcoder");
            }
            rtmp_server::cluster::NodeHeartbeat beat;
            beat.id = field("id");
            beat.role = *role;
            beat.address = field("address");
            beat.region = field("region");
            beat.capacity_viewers = number("capacity_viewers");
            beat.active_viewers = number("active_viewers");
            beat.active_publishers = number("active_publishers");
            beat.draining = field("draining") == "true" || field("draining") == "1";
            const auto now = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            auto status = node_registry.heartbeat(beat, now);
            if (!status) return status.error();
            return cluster_node_json(status.value());
        },
        [&node_registry](std::string_view id) -> rtmp_server::core::Result<void> {
            return node_registry.remove(id);
        },
        [&node_registry](std::string_view region) -> rtmp_server::core::Result<std::string> {
            const auto now = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const auto node = node_registry.locate(region, now);
            if (!node) {
                return rtmp_server::core::Error(rtmp_server::core::ErrorCode::NotFound,
                                                rtmp_server::core::ErrorCategory::Configuration,
                                                "no healthy node can take a new viewer");
            }
            return cluster_node_json(*node);
        },
        [&node_registry](std::string_view application, std::string_view stream,
                         std::string_view region,
                         std::string_view format) -> rtmp_server::core::Result<std::string> {
            const auto now = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const auto node = node_registry.locate(region, now);
            if (!node) {
                return rtmp_server::core::Error(rtmp_server::core::ErrorCode::NotFound,
                                                rtmp_server::core::ErrorCategory::Configuration,
                                                "no healthy node can take a new viewer");
            }
            // The address a node heartbeats with is a bare host (see
            // STREAMFORGE_NODE_ADDRESS); the scheme is always https, matching
            // every deployment's viewer-facing entry (Caddy terminates TLS on
            // both an origin and an edge).
            std::string base = node->address;
            if (!base.starts_with("http://") && !base.starts_with("https://")) {
                base = "https://" + base;
            }
            if (format == "dash") {
                return base + "/dash/" + std::string(application) + "/" + std::string(stream) +
                       "/manifest.mpd";
            }
            return base + "/hls/" + std::string(application) + "/" + std::string(stream) +
                   "/index.m3u8";
        },
        [&node_registry]() -> rtmp_server::core::Result<std::string> {
            const auto now = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const auto snapshot = node_registry.capacity(now);
            std::ostringstream os;
            os << R"({"healthy_edges":)" << snapshot.healthy_edges << R"(,"capacity_viewers":)"
               << snapshot.capacity_viewers << R"(,"active_viewers":)" << snapshot.active_viewers
               << R"(,"utilization":)" << snapshot.utilization << R"(,"scale_out_recommended":)"
               << (snapshot.scale_out_recommended ? "true" : "false") << '}';
            return os.str();
        });

    // Stream targets: relay to another origin, or push to an external ingest.
    management_api.set_stream_target_handlers(
        [&stream_targets](std::string_view application) -> rtmp_server::core::Result<std::string> {
            std::ostringstream os;
            os << R"({"items":[)";
            const auto targets = stream_targets.list(application);
            for (std::size_t i = 0; i < targets.size(); ++i) {
                if (i) os << ',';
                os << stream_target_json(targets[i]);
            }
            os << "]}";
            return os.str();
        },
        [&stream_targets](std::string_view application, std::string_view stream,
                          std::string_view name,
                          const std::unordered_map<std::string, std::string>& fields)
            -> rtmp_server::core::Result<std::string> {
            const auto field = [&fields](const char* key) -> std::string {
                const auto it = fields.find(key);
                return it == fields.end() ? std::string{} : it->second;
            };
            rtmp_server::relay::StreamTargetConfig config;
            config.application = std::string(application);
            config.stream = std::string(stream);
            config.name = std::string(name);
            config.url = field("url");
            config.relay = field("relay") == "true" || field("relay") == "1";
            // Absent means enabled: a target is created to be used, and the
            // PATCH route exists for turning one off.
            config.enabled = field("enabled") != "false" && field("enabled") != "0";
            auto status = stream_targets.upsert(config);
            if (!status) return status.error();
            return stream_target_json(status.value());
        },
        [&stream_targets](std::string_view application, std::string_view stream,
                          std::string_view name) -> rtmp_server::core::Result<void> {
            return stream_targets.remove(application, stream, name);
        },
        [&stream_targets](std::string_view application, std::string_view stream,
                          std::string_view name, bool enabled) -> rtmp_server::core::Result<std::string> {
            auto status = stream_targets.set_enabled(application, stream, name, enabled);
            if (!status) return status.error();
            return stream_target_json(status.value());
        });

    const auto backup_publisher_json = [](const rtmp_server::relay::BackupPublisherStatus& target) {
        const auto state = [&] {
            switch (target.state) {
                case rtmp_server::relay::BackupPublisherState::Standby: return "standby";
                case rtmp_server::relay::BackupPublisherState::Connecting: return "connecting";
                case rtmp_server::relay::BackupPublisherState::Active: return "active";
                case rtmp_server::relay::BackupPublisherState::Error: return "error";
                case rtmp_server::relay::BackupPublisherState::Disabled: return "disabled";
            }
            return "standby";
        }();
        std::ostringstream os;
        os << R"({"id":")" << json_escape(target.application + ":" + target.stream)
           << R"(","application":")" << json_escape(target.application) << R"(","stream":")"
           << json_escape(target.stream) << R"(","url":")" << json_escape(target.url_redacted)
           << R"(","enabled":)" << (target.enabled ? "true" : "false") << R"(,"state":")" << state
           << R"(","detail":")" << json_escape(target.detail) << R"(","activations":)"
           << target.activations << R"(,"bytes_in":)" << target.bytes_in << '}';
        return os.str();
    };
    management_api.set_backup_publisher_handlers(
        [&backup_publishers, backup_publisher_json](std::string_view application)
            -> rtmp_server::core::Result<std::string> {
            std::ostringstream os;
            os << R"({"items":[)";
            const auto items = backup_publishers.list(application);
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i) os << ',';
                os << backup_publisher_json(items[i]);
            }
            os << "]}";
            return os.str();
        },
        [&backup_publishers, backup_publisher_json](
            std::string_view application, std::string_view stream,
            const std::unordered_map<std::string, std::string>& fields)
            -> rtmp_server::core::Result<std::string> {
            const auto field = [&fields](const char* key) -> std::string {
                const auto it = fields.find(key);
                return it == fields.end() ? std::string{} : it->second;
            };
            rtmp_server::relay::BackupPublisherConfig config;
            config.application = std::string(application);
            config.stream = std::string(stream);
            config.backup_url = field("backup_url");
            config.enabled = field("enabled") != "false" && field("enabled") != "0";
            const auto failover_after = field("failover_after_seconds");
            if (!failover_after.empty()) {
                errno = 0;
                const auto value = std::strtoull(failover_after.c_str(), nullptr, 10);
                if (errno == 0 && value > 0 &&
                    value <= std::numeric_limits<std::uint32_t>::max()) {
                    config.failover_after_seconds = static_cast<std::uint32_t>(value);
                }
            }
            auto status = backup_publishers.upsert(config);
            if (!status) return status.error();
            return backup_publisher_json(status.value());
        },
        [&backup_publishers](std::string_view application,
                             std::string_view stream) -> rtmp_server::core::Result<void> {
            return backup_publishers.remove(application, stream);
        });

#ifdef RTMP_NATIVE_TRANSCODE
    // Source-transcode jobs: pull an external URL (rtmp:// or an http(s)
    // .m3u8/.ts) and transcode it in-process via HlsSourcePuller -- no
    // ffmpeg subprocess. Each rendition writes straight into its own
    // SegmentStore, which is registered with hls_handler exactly like a
    // normal published stream's segments, so the existing HLS-serving path
    // needs no changes to pick up a source job's output.
    rtmp_server::transcoding::native::SourceJobManager::Hooks source_hooks;
    source_hooks.set_renditions = [&hls_handler](const std::string& application,
                                                 const std::string& master,
                                                 std::vector<rtmp_server::hls::Rendition> renditions) {
        hls_handler.set_renditions(application, master, std::move(renditions));
    };
    source_hooks.register_output = [&hls_handler](const std::string& application,
                                                   const std::string& stream,
                                                   std::shared_ptr<rtmp_server::hls::SegmentStore> store) {
        hls_handler.register_stream(application, stream, std::move(store));
    };
    source_hooks.unregister_output = [&hls_handler](const std::string& application,
                                                     const std::string& stream) {
        hls_handler.unregister_stream(application, stream);
    };
    rtmp_server::transcoding::native::SourceJobManager::Options source_job_options;
    // Same operator-facing knob as the ingest side (config.transcode_cpu_
    // reservation_percent): confines every job's scale+encode work (and its
    // encoders' own internal threads) to the reserved slice so RTMP ingest/
    // HTTP/admin traffic can never starve an in-flight transcode, and vice
    // versa. 0 (default) keeps today's behaviour of sizing from every core.
    source_job_options.transcode_cpu_reservation_percent = config.transcode_cpu_reservation_percent;
    // 6 s output segments for pulled/transcoded sources, matching the RTMP
    // ingest path above. On a large single-box audience this thirds each
    // viewer's playlist+segment request rate versus 2 s segments; the
    // per-rendition byte budget grows in step so a high-bitrate rendition
    // still evicts on segment count, not on the cap.
    source_job_options.target_duration_seconds = 6;
    // Deeper live window (10 x 6 s): a pulled source is not under this
    // server's control and can stall or hop CDN backends at any moment; a
    // 60 s player cushion keeps playback smooth across that.
    source_job_options.live_window_segments = 10;
    source_job_options.retention_grace_segments = 6;
    source_job_options.max_total_bytes_per_rendition = 256u * 1024u * 1024u;
    rtmp_server::transcoding::native::SourceJobManager source_job_manager(std::move(source_hooks), store.get(),
                                                                          source_job_options);
    source_job_manager.load_from_store();

    const auto source_job_json =
        [&hls_handler, &edge_viewers](const rtmp_server::transcoding::native::SourceJobSnapshot& job) {
            // Source-transcode renditions never register with LiveFanout/
            // StreamManager (no RTMP subscriber exists for a pull-based
            // job's own output), so their audience is only visible in HLS
            // request traffic. Prefer the cache edge's count: the origin
            // sees only the ~1 req/s Varnish could not answer from cache,
            // so aggregate_link_stats() reports about one viewer for a link
            // with ten thousand. It stays as the fallback for a deployment
            // with no edge accounting, flagged by delivery_stats_available.
            const auto edge = edge_viewers.snapshot();
            const auto origin_stats = hls_handler.aggregate_link_stats(job.application, job.name);

            // Players fetch a rendition's index.m3u8, so the edge keys each
            // session under that rendition's own stream name; the job's own
            // name is included for a rendition published under it directly.
            std::vector<std::string> link_keys;
            link_keys.reserve(job.renditions.size() + 1);
            link_keys.push_back(job.name);
            for (const auto& rendition : job.renditions) {
                if (rendition.output_stream != job.name) link_keys.push_back(rendition.output_stream);
            }
            const auto edge_value = [&](const std::unordered_map<std::string, std::uint64_t>& values,
                                        const std::string& stream) -> std::uint64_t {
                const auto it = values.find(job.application + "/" + stream);
                return it == values.end() ? 0 : it->second;
            };
            const auto edge_total = [&](const std::unordered_map<std::string, std::uint64_t>& values) {
                std::uint64_t total = 0;
                for (const auto& key : link_keys) total += edge_value(values, key);
                return total;
            };

            const std::uint64_t viewer_count =
                edge.fresh ? edge_total(edge.viewers)
                           : static_cast<std::uint64_t>(origin_stats.viewer_count);
            const std::uint64_t bytes_total =
                edge.fresh ? edge_total(edge.bytes_total) : origin_stats.bytes_total;
            const std::uint64_t bitrate_bps = edge.fresh ? edge_total(edge.bitrate_bps) : 0;

            std::ostringstream os;
            os << R"({"id":")" << json_escape(job.application + ":" + job.name) << R"(","application":")"
               << json_escape(job.application) << R"(","name":")" << json_escape(job.name)
               << R"(","source_url":")" << json_escape(job.source_url) << R"(","template_name":")"
               << json_escape(job.template_name) << R"(","master_hls_path":")"
               << json_escape(job.master_hls_path) << R"(","status":")" << json_escape(job.status)
               << R"(","detail":")" << json_escape(job.detail) << R"(","enabled":)"
               << (job.enabled ? "true" : "false") << R"(,"auto_restart":)"
               << (job.auto_restart ? "true" : "false") << R"(,"restart_delay_seconds":)"
               << job.restart_delay_seconds << R"(,"bytes_total":)" << bytes_total
               << R"(,"viewer_count":)" << viewer_count
               << R"(,"hls_egress_bitrate_bps":)" << bitrate_bps
               << R"(,"delivery_stats_available":)" << (edge.fresh ? "true" : "false")
               << R"(,"outputs":[)";
            for (std::size_t i = 0; i < job.renditions.size(); ++i) {
                const auto& r = job.renditions[i];
                if (i) os << ',';
                // Per-rendition delivery, so each rung of the ladder shows
                // its own audience rather than only the job's total.
                os << R"({"name":")" << json_escape(r.name) << R"(","stream":")"
                   << json_escape(r.output_stream) << R"(","video_codec":"h264","video_bitrate":)"
                   << r.video_bitrate << R"(,"width":)" << r.width << R"(,"height":)" << r.height
                   << R"(,"viewer_count":)" << (edge.fresh ? edge_value(edge.viewers, r.output_stream) : 0)
                   << R"(,"bytes_total":)" << (edge.fresh ? edge_value(edge.bytes_total, r.output_stream) : 0)
                   << R"(,"hls_egress_bitrate_bps":)"
                   << (edge.fresh ? edge_value(edge.bitrate_bps, r.output_stream) : 0) << "}";
            }
            os << "]}";
            return os.str();
        };

    // Transcoding assignments: which published streams get a re-encoded
    // rendition ladder. Persisted in the same table the panel already posts
    // to; every route answered 503 before this wiring existed.
    management_api.set_transcoding_assignment_handlers(
        [&ingest_transcoder](std::string_view application) -> rtmp_server::core::Result<std::string> {
            std::ostringstream os;
            os << R"({"items":[)";
            const auto assignments = ingest_transcoder.list(application);
            for (std::size_t i = 0; i < assignments.size(); ++i) {
                const auto& assignment = assignments[i];
                if (i) os << ',';
                os << R"({"id":")" << json_escape(assignment.application + ":" + assignment.source_stream)
                   << R"(","application":")" << json_escape(assignment.application)
                   << R"(","source_stream":")" << json_escape(assignment.source_stream)
                   << R"(","template_name":")" << json_escape(assignment.template_name)
                   << R"(","master_hls_path":")" << json_escape(assignment.master_hls_path)
                   << R"(","active":)" << (assignment.active ? "true" : "false")
                   << R"(,"frames_in":)" << assignment.status.frames_in
                   << R"(,"frames_dropped":)" << assignment.status.queue.dropped
                   << R"(,"resyncs":)" << assignment.status.queue.resyncs
                   << R"(,"detail":")" << json_escape(assignment.status.detail)
                   << R"(","outputs":[)";
                for (std::size_t j = 0; j < assignment.renditions.size(); ++j) {
                    const auto& rendition = assignment.renditions[j];
                    if (j) os << ',';
                    os << R"({"name":")" << json_escape(rendition.name) << R"(","stream":")"
                       << json_escape(rendition.output_stream)
                       << R"(","video_codec":"h264","video_bitrate":)" << rendition.video_bitrate
                       << R"(,"audio_bitrate":)" << rendition.audio_bitrate << R"(,"width":)"
                       << rendition.width << R"(,"height":)" << rendition.height << '}';
                }
                os << "]}";
            }
            os << "]}";
            return os.str();
        },
        [&ingest_transcoder](std::string_view application, std::string_view source_stream,
                             std::string_view template_name,
                             std::string_view rules) -> rtmp_server::core::Result<std::string> {
            return ingest_transcoder.upsert(application, source_stream, template_name, rules);
        },
        [&ingest_transcoder](std::string_view application,
                             std::string_view source_stream) -> rtmp_server::core::Result<void> {
            return ingest_transcoder.remove(application, source_stream);
        });

    management_api.set_source_job_handlers(
        [&source_job_manager, source_job_json](std::string_view application)
            -> rtmp_server::core::Result<std::string> {
            std::ostringstream os;
            os << R"({"items":[)";
            const auto jobs = source_job_manager.list(std::string(application));
            for (std::size_t i = 0; i < jobs.size(); ++i) {
                if (i) os << ',';
                os << source_job_json(jobs[i]);
            }
            os << "]}";
            return os.str();
        },
        [&source_job_manager, source_job_json](
            std::string_view application, std::string_view name, std::string_view source_url,
            std::string_view template_name, std::string_view rules, bool auto_restart,
            std::uint32_t restart_delay_seconds) -> rtmp_server::core::Result<std::string> {
            auto renditions =
                rtmp_server::transcoding::native::parse_source_job_renditions(rules);
            if (!renditions) return renditions.error();

            rtmp_server::transcoding::native::SourceJobConfig cfg;
            cfg.application = std::string(application);
            cfg.name = std::string(name);
            cfg.source_url = std::string(source_url);
            cfg.template_name = std::string(template_name);
            cfg.rules = std::string(rules);
            cfg.auto_restart = auto_restart;
            cfg.restart_delay_seconds = restart_delay_seconds;
            cfg.renditions = std::move(renditions).value();
            // Bounds the decode-once/encode-per-rendition fan-out started by
            // one job; matches the deleted ffmpeg-based manager's intent
            // without a dedicated config knob (native pipeline has no
            // per-process ffmpeg cost to size against).
            constexpr std::size_t kMaxOutputsPerJob = 8;
            if (cfg.renditions.size() > kMaxOutputsPerJob) {
                return rtmp_server::core::Error(rtmp_server::core::ErrorCode::InvalidConfiguration,
                                                rtmp_server::core::ErrorCategory::Configuration,
                                                "too many renditions for one source job");
            }
            auto snapshot = source_job_manager.create(cfg);
            if (!snapshot) return snapshot.error();
            return source_job_json(snapshot.value());
        },
        [&source_job_manager](std::string_view application,
                              std::string_view name) -> rtmp_server::core::Result<void> {
            if (!source_job_manager.remove(std::string(application), std::string(name))) {
                return rtmp_server::core::Error(rtmp_server::core::ErrorCode::NotFound,
                                                rtmp_server::core::ErrorCategory::Configuration,
                                                "no such source job");
            }
            return {};
        },
        [&source_job_manager, source_job_json](
            std::string_view application, std::string_view name,
            bool enabled) -> rtmp_server::core::Result<std::string> {
            auto snapshot = source_job_manager.set_enabled(std::string(application),
                                                            std::string(name), enabled);
            if (!snapshot) return snapshot.error();
            return source_job_json(snapshot.value());
        },
        [&source_job_manager, source_job_json](
            std::string_view application,
            std::string_view name) -> rtmp_server::core::Result<std::string> {
            auto snapshot = source_job_manager.restart(std::string(application), std::string(name));
            if (!snapshot) return snapshot.error();
            return source_job_json(snapshot.value());
        });
#endif

    dash_handler.set_next([&management_api](const rtmp_server::control::HttpRequest& request) {
        return management_api.handle(request);
    });
    hls_handler.set_next([&dash_handler](const rtmp_server::control::HttpRequest& request) {
        return dash_handler.handle(request);
    });
    // Origin HTTP delivery for /hls plus the management API.
    //
    // Event-driven, not one thread per connection: connection count is
    // decoupled from thread count, so the origin's capacity is set by file
    // descriptors and bandwidth rather than by a pool size. The previous
    // blocking server pinned a worker to a connection for its whole life,
    // which made the pool the real viewer ceiling and forced two workarounds
    // that are no longer needed -- an oversized pool (cores * 32) and
    // keep-alive disabled entirely.
    //
    // One loop per core is the right number here and multiplying it buys
    // nothing: a loop is CPU-bound on its own thread and never blocks on a
    // socket, so extra loops would only add scheduler pressure. On Linux each
    // loop takes its own SO_REUSEPORT listener, so accepts scale with cores
    // instead of funnelling through a single queue.
    //
    // Handlers run on a loop thread, so they must not block. HlsHttpHandler
    // satisfies that: its request counters are atomics, its stream registry is
    // read under a shared lock, and its viewer-session accounting is sharded
    // (see hls_http_handler.hpp and viewer_session_tracker.hpp).
    rtmp_server::control::AsyncHttpServerOptions api_server_options;
    api_server_options.bind_address = config.api_bind_address;
    api_server_options.port = config.api_port;
    api_server_options.listen_backlog = 65'535;
    api_server_options.event_loops =
        std::max<std::size_t>(1, std::thread::hardware_concurrency());
    // Unbounded by request, as before: the process fd limit (LimitNOFILE in
    // deploy/systemd/rtmp-server.service) is the real ceiling, and refusing a
    // loopback request for load would turn a join spike into origin 503s even
    // though every segment body is already cache-hot.
    api_server_options.max_connections = 0;
    api_server_options.max_header_bytes = 16 * 1024;
    api_server_options.max_body_bytes = 128 * 1024;
    // Now safe to leave on, and worth it: an idle keep-alive connection costs
    // an entry in an interest set rather than a thread, so the cache's
    // backend connections no longer starve playlist and segment requests. This
    // removes a full TCP setup per loopback request at every viewer's segment
    // cadence.
    api_server_options.enable_keep_alive = true;
    // The old 1000-request cap existed to bound how long one client could hold
    // a worker thread. No thread is held now, so the bound's only remaining
    // job is to recycle a connection occasionally -- and the peers here are
    // Caddy and Varnish over loopback, which reuse a connection for minutes at
    // every viewer's segment cadence. Recycling every 1000 requests would
    // reintroduce exactly the connection churn keep-alive is here to remove.
    api_server_options.max_requests_per_connection = 1'000'000;
    rtmp_server::control::AsyncHttpServer api_server(api_server_options);
    api_server.set_handler([&hls_handler](const rtmp_server::control::HttpRequest& request) {
        return hls_handler.handle(request);
    });
    if (!api_server.start()) {
        std::fprintf(stderr, "failed to bind management API on %s:%u\n", config.api_bind_address.c_str(),
                     static_cast<unsigned>(config.api_port));
        return EXIT_FAILURE;
    }

    // Every connection each worker's ring accepts is driven through an RTMP
    // handshake automatically (IoUringEventLoop::start_handshake, wired from
    // handle_accept_completion in src/io/io_uring/event_loop.cpp) before any
    // higher-level RTMP processing begins — see docs/rtmp-handshake.md.
    // This origin's own heartbeat. It is a cluster member like any other: a
    // deployment with no edges still needs /v1/cluster/locate to answer, and
    // the panel needs to see the origin's own publisher count next to its
    // edges'. Identity comes from the environment (see environment_or) so one
    // server.yaml can be copied across machines.
    const std::string node_id =
        environment_or("STREAMFORGE_NODE_ID", "origin-" + local_hostname());
    const std::string node_region = environment_or("STREAMFORGE_NODE_REGION", "");
    const std::string node_address =
        environment_or("STREAMFORGE_NODE_ADDRESS", config.public_rtmp_hostname);
    std::jthread origin_heartbeat([&](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            rtmp_server::cluster::NodeHeartbeat beat;
            beat.id = node_id;
            beat.role = rtmp_server::cluster::NodeRole::Origin;
            beat.address = node_address;
            beat.region = node_region;
            beat.active_publishers = static_cast<std::uint32_t>(stream_registry.snapshot().size());
            const auto edge = edge_viewers.snapshot();
            // Only the cache tier can count HLS viewers (the origin sees ~1
            // request per second per link however large the audience), so an
            // origin with no edge accounting reports zero rather than a number
            // that means something else.
            if (edge.fresh) beat.active_viewers = static_cast<std::uint32_t>(edge.total_viewers);
            const auto now = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            if (auto beaten = node_registry.heartbeat(beat, now); !beaten) {
                RTMP_LOG(rtmp_server::observability::LogLevel::Warn, "cluster",
                         "origin heartbeat failed", {{"error", beaten.error().message()}});
            }
            // Well inside NodeRegistryOptions::heartbeat_timeout (30 s), so a
            // single slow tick never marks this origin unhealthy. Slept in
            // short slices so shutdown does not wait out an interval.
            for (int i = 0; i < 20 && !stop.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    });

    g_pool = &pool;
    install_signal_handlers();

    auto run_result = pool.run();
    g_pool = nullptr;
    api_server.stop();

    if (!run_result) {
        std::fprintf(stderr, "worker pool exited with error: %s\n",
                      run_result.error().message().c_str());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
