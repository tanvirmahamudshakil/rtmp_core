#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rtmp_server/authentication/rtmp_authenticator.hpp"
#include "rtmp_server/control/edge_viewer_stats.hpp"
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
#include "rtmp_server/transcoding/preset.hpp"
#ifdef RTMP_NATIVE_TRANSCODE
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
    services.recorder_factory =
        [&hls_handler](std::string_view application,
                       std::string_view stream) -> std::shared_ptr<rtmp_server::protocol::commands::RecorderSink> {
            rtmp_server::hls::SegmentStoreConfig store_config;
            store_config.live_window_segments = 6;
            store_config.retention_grace_segments = 6;
            store_config.max_total_bytes = 128u * 1024u * 1024u;
            store_config.target_duration_seconds = 2;
            auto store = std::make_shared<rtmp_server::hls::SegmentStore>(store_config);

            rtmp_server::hls::SegmenterConfig segmenter_config;
            segmenter_config.target_duration = std::chrono::seconds(2);
            segmenter_config.max_segment_duration = std::chrono::seconds(8);
            segmenter_config.max_segment_bytes = 16u * 1024u * 1024u;

            hls_handler.register_stream(std::string(application), std::string(stream), store);
            return std::make_shared<rtmp_server::hls::StreamSink>(std::move(store),
                                                                  std::move(segmenter_config));
        };

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
        [&hls_handler](std::string_view application, std::string_view stream) {
            hls_handler.unregister_stream(std::string(application), std::string(stream));
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

    hls_handler.set_next([&management_api](const rtmp_server::control::HttpRequest& request) {
        return management_api.handle(request);
    });
    // Caddy drains these loopback responses and performs the public
    // asynchronous client I/O. HLS players request a newly published segment
    // in synchronized bursts, so size the blocking loopback pool from the
    // machine rather than imposing the old fixed 16-request bottleneck.
    //
    // The pool is one blocking worker per in-flight request (keep-alive is
    // off below), so it also bounds how many concurrent cache MISSes / fresh
    // per-viewer playlist redirects the origin can serve at once. A join
    // stampede on a large audience needs thousands of those in flight for a
    // few milliseconds each, so the ceiling is deliberately far above the
    // core count -- idle worker threads cost only their (lazily faulted)
    // stack, while too low a ceiling turns a join spike into origin 503s
    // even though every segment/playlist body is already cache-hot. Paired
    // with an unbounded pending queue below, the origin never rejects a
    // loopback request for load: it either has a free worker or the request
    // waits microseconds for one, with no audience-size cap.
    const auto hardware_threads = std::max(1U, std::thread::hardware_concurrency());
    const auto hls_http_workers =
        std::clamp<std::size_t>(static_cast<std::size_t>(hardware_threads) * 256, 2048, 65536);
    rtmp_server::control::HttpServerOptions api_server_options{
        config.api_bind_address, config.api_port, 65'535, hls_http_workers,
        std::numeric_limits<std::size_t>::max(), 16 * 1024, 128 * 1024};
    // This server assigns one blocking worker to a connection for its entire
    // keep-alive lifetime. A cache restart can leave hundreds of backend
    // connections queued while every worker waits idle on an earlier one,
    // starving health, playlist, and segment requests despite an otherwise
    // idle machine. Loopback TCP setup is cheap; close after each response so
    // workers are scheduled per request and no idle connection owns a slot.
    api_server_options.enable_keep_alive = false;
    rtmp_server::control::HttpServer api_server(api_server_options);
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
