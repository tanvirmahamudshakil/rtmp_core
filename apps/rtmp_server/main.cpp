#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "rtmp_server/authentication/rtmp_authenticator.hpp"
#include "rtmp_server/control/hls_http_handler.hpp"
#include "rtmp_server/control/http_server.hpp"
#include "rtmp_server/control/management_api.hpp"
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
#include "rtmp_server/transcoding/supervisor.hpp"
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

std::string assignment_json(const rtmp_server::persistence::TranscodingAssignmentRow& row) {
    auto parsed = rtmp_server::transcoding::PresetCatalogue::parse(row.rules);
    std::ostringstream json;
    json << R"({"application":")" << json_escape(row.application)
         << R"(","source_stream":")" << json_escape(row.source_stream)
         << R"(","template_name":")" << json_escape(row.template_name)
         << R"(","master_hls_path":"/hls/)" << json_escape(row.application) << "/"
         << json_escape(row.source_stream) << R"(/master.m3u8","outputs":[)";
    bool first = true;
    if (parsed) {
        for (const auto& preset : parsed.value().match(row.application, row.source_stream)) {
            if (!first) json << ",";
            first = false;
            json << R"({"name":")" << json_escape(preset.name)
                 << R"(","stream":")" << json_escape(preset.outgoing_stream_name)
                 << R"(","video_codec":")" << rtmp_server::transcoding::to_string(preset.video_codec)
                 << R"(","video_bitrate":)" << preset.video_bitrate
                 << R"(,"width":)" << preset.width.value_or(0)
                 << R"(,"height":)" << preset.height.value_or(0) << "}";
        }
    }
    json << "]}";
    return json.str();
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

    std::unique_ptr<rtmp_server::transcoding::TranscoderSupervisor> transcoder;
    if (config.transcoding_enabled) {
        auto catalogue_result =
            rtmp_server::transcoding::PresetCatalogue::load(config.transcoding_preset_file);
        if (!catalogue_result) {
            std::fprintf(stderr, "failed to load transcoding presets '%s': %s\n",
                         config.transcoding_preset_file.c_str(),
                         catalogue_result.error().message().c_str());
            return EXIT_FAILURE;
        }
        auto assignments_result = store->load_transcoding_assignments();
        if (!assignments_result) {
            std::fprintf(stderr, "failed to load transcoding assignments: %s\n",
                         assignments_result.error().message().c_str());
            return EXIT_FAILURE;
        }
        for (const auto& assignment : assignments_result.value()) {
            auto parsed = rtmp_server::transcoding::PresetCatalogue::parse(assignment.rules);
            if (!parsed || parsed.value().rules().size() != 1) {
                std::fprintf(stderr, "invalid persisted transcoding assignment for %s/%s\n",
                             assignment.application.c_str(), assignment.source_stream.c_str());
                return EXIT_FAILURE;
            }
            auto rule = parsed.value().rules().front();
            if (rule.application != assignment.application ||
                rule.source_stream != assignment.source_stream) {
                std::fprintf(stderr, "persisted transcoding assignment identity mismatch for %s/%s\n",
                             assignment.application.c_str(), assignment.source_stream.c_str());
                return EXIT_FAILURE;
            }
            catalogue_result.value().upsert_rule(std::move(rule));
        }
        rtmp_server::transcoding::SupervisorOptions options;
        options.enabled = true;
        options.rtmp_port = config.rtmp_port;
        options.ffmpeg_path = config.transcoding_ffmpeg_path;
        options.max_active_jobs = config.transcoding_max_active_jobs;
        options.max_outputs_per_job = config.transcoding_max_outputs_per_job;
        options.max_restart_attempts = config.transcoding_max_restart_attempts;
        transcoder = std::make_unique<rtmp_server::transcoding::TranscoderSupervisor>(
            std::move(options), std::move(catalogue_result).value());
        transcoder->set_prepare_output(
            [&stream_manager](std::string_view application, std::string_view output_stream) {
                if (stream_manager.find_stream(application, output_stream)) return true;
                return stream_manager.create_stream(application, std::string(output_stream), false).ok();
            });
        transcoder->set_renditions_ready(
            [&hls_handler](std::string_view application, std::string_view source_stream,
                           const std::vector<rtmp_server::transcoding::Preset>& presets) {
                std::vector<rtmp_server::hls::Rendition> renditions;
                renditions.reserve(presets.size());
                for (const auto& preset : presets) {
                    rtmp_server::hls::Rendition rendition;
                    rendition.uri = "../" + preset.outgoing_stream_name + "/index.m3u8";
                    rendition.average_bandwidth = preset.video_bitrate + preset.audio_bitrate;
                    rendition.bandwidth = (rendition.average_bandwidth * 125 + 99) / 100;
                    rendition.codecs = "avc1.64001f,mp4a.40.2";
                    rendition.width = preset.width.value_or(0);
                    rendition.height = preset.height.value_or(0);
                    rendition.name = preset.name;
                    renditions.push_back(std::move(rendition));
                }
                hls_handler.set_renditions(std::string(application), std::string(source_stream),
                                           std::move(renditions));
            });
        auto start_result = transcoder->start();
        if (!start_result) {
            std::fprintf(stderr, "failed to start transcoder supervisor: %s\n",
                         start_result.error().message().c_str());
            return EXIT_FAILURE;
        }
        services.publish_start_handler =
            [&transcoder](const rtmp_server::protocol::commands::StreamRegistration& registration) {
                transcoder->on_publish_started(registration);
            };
        services.publish_stop_handler =
            [&transcoder](std::string_view, std::uint64_t connection_id) {
                transcoder->on_publish_stopped(connection_id);
            };
    }
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
    management_api.set_stream_deleted_handler(
        [&hls_handler](std::string_view application, std::string_view stream) {
            hls_handler.unregister_stream(std::string(application), std::string(stream));
        });
    if (transcoder) {
        const auto backend_capabilities = transcoder->capabilities();
        management_api.set_transcoding_status_provider(
            [&transcoder, backend_capabilities] {
                std::ostringstream json;
                json << R"({"enabled":true,"capabilities":[)";
                for (std::size_t i = 0; i < backend_capabilities.size(); ++i) {
                    if (i > 0) json << ",";
                    const auto& capability = backend_capabilities[i];
                    json << R"({"backend":")"
                         << rtmp_server::transcoding::to_string(capability.kind)
                         << R"(","available":)" << (capability.available ? "true" : "false")
                         << R"(,"detail":")" << capability.detail << "\"}";
                }
                json << R"(],"jobs":[)";
                const auto jobs = transcoder->snapshot();
                for (std::size_t i = 0; i < jobs.size(); ++i) {
                    if (i > 0) json << ",";
                    const auto& job = jobs[i];
                    json << R"({"application":")" << job.application
                         << R"(","source_stream":")" << job.source_stream
                         << R"(","pid":)" << job.process_id
                         << R"(,"running":)" << (job.running ? "true" : "false")
                         << R"(,"restart_attempts":)" << job.restart_attempts
                         << R"(,"outputs":[)";
                    for (std::size_t output = 0; output < job.output_streams.size(); ++output) {
                        if (output > 0) json << ",";
                        json << "\"" << job.output_streams[output] << "\"";
                    }
                    json << "]}";
                }
                json << "]}";
                return json.str();
            });
        management_api.set_transcoding_assignment_handlers(
            [&store](std::string_view application) -> rtmp_server::core::Result<std::string> {
                auto rows = store->load_transcoding_assignments();
                if (!rows) return rows.error();
                std::ostringstream json;
                json << R"({"items":[)";
                bool first = true;
                for (const auto& row : rows.value()) {
                    if (row.application != application) continue;
                    if (!first) json << ",";
                    first = false;
                    json << assignment_json(row);
                }
                json << "]}";
                return json.str();
            },
            [&store, &stream_manager, &stream_registry, &transcoder, &config](
                std::string_view application, std::string_view source_stream,
                std::string_view template_name,
                std::string_view rules) -> rtmp_server::core::Result<std::string> {
                if (!stream_manager.find_stream(application, source_stream)) {
                    return rtmp_server::core::Error(
                        rtmp_server::core::ErrorCode::NotFound,
                        rtmp_server::core::ErrorCategory::Configuration,
                        "source stream not found");
                }
                auto parsed = rtmp_server::transcoding::PresetCatalogue::parse(rules);
                if (!parsed || parsed.value().rules().size() != 1) {
                    return parsed ? rtmp_server::core::Error(
                                        rtmp_server::core::ErrorCode::InvalidConfiguration,
                                        rtmp_server::core::ErrorCategory::Configuration,
                                        "assignment must contain exactly one source rule")
                                  : parsed.error();
                }
                auto rule = parsed.value().rules().front();
                if (rule.application != application || rule.source_stream != source_stream ||
                    rule.presets.empty() ||
                    rule.presets.size() > config.transcoding_max_outputs_per_job) {
                    return rtmp_server::core::Error(
                        rtmp_server::core::ErrorCode::InvalidConfiguration,
                        rtmp_server::core::ErrorCategory::Configuration,
                        "assignment identity is invalid, empty or exceeds the output limit");
                }
                for (const auto& preset : rule.presets) {
                    const bool supported_video =
                        preset.video_codec == rtmp_server::transcoding::VideoCodec::H264 ||
                        preset.video_codec == rtmp_server::transcoding::VideoCodec::Passthrough ||
                        preset.video_codec == rtmp_server::transcoding::VideoCodec::Disabled;
                    const bool supported_audio =
                        preset.audio_codec == rtmp_server::transcoding::AudioCodec::Aac ||
                        preset.audio_codec == rtmp_server::transcoding::AudioCodec::Passthrough ||
                        preset.audio_codec == rtmp_server::transcoding::AudioCodec::Disabled;
                    const bool licensed_backend =
                        preset.backend == rtmp_server::transcoding::BackendKind::Beamr ||
                        preset.backend == rtmp_server::transcoding::BackendKind::MainConcept;
                    if (!supported_video || !supported_audio || licensed_backend) {
                        return rtmp_server::core::Error(
                            rtmp_server::core::ErrorCode::InvalidConfiguration,
                            rtmp_server::core::ErrorCategory::Configuration,
                            "current RTMP/HLS output supports H.264/AAC with Default, NVENC or QuickSync");
                    }
                }

                rtmp_server::persistence::TranscodingAssignmentRow row;
                row.application = std::string(application);
                row.source_stream = std::string(source_stream);
                row.template_name = std::string(template_name);
                row.rules = std::string(rules);
                auto stored = store->upsert_transcoding_assignment(row);
                if (!stored) return stored.error();

                transcoder->apply_rule(std::move(rule));
                const auto registrations = stream_registry.snapshot();
                const auto live = std::ranges::find_if(registrations, [&](const auto& registration) {
                    return registration.app == application && registration.stream_key == source_stream;
                });
                if (live != registrations.end()) transcoder->on_publish_started(*live);
                return assignment_json(row);
            },
            [&store, &transcoder](std::string_view application,
                                  std::string_view source_stream) -> rtmp_server::core::Result<void> {
                auto removed = store->delete_transcoding_assignment(application, source_stream);
                if (!removed) return removed.error();
                transcoder->remove_rule(std::string(application), std::string(source_stream));
                return {};
            });
    }
#ifdef RTMP_NATIVE_TRANSCODE
    // Source-transcode jobs: pull an external URL (rtmp:// or an http(s) .m3u8),
    // transcode it per a template with the in-process FFmpeg-free pipeline, and
    // re-serve the renditions as one adaptive master .m3u8. Each rendition's
    // segment store is registered with the HLS handler for delivery.
    rtmp_server::transcoding::native::SourceJobManager::Hooks source_hooks;
    source_hooks.register_store = [&hls_handler](const std::string& application,
                                                 const std::string& stream,
                                                 std::shared_ptr<rtmp_server::hls::SegmentStore> store) {
        hls_handler.register_stream(application, stream, std::move(store));
    };
    source_hooks.unregister_store = [&hls_handler](const std::string& application,
                                                   const std::string& stream) {
        hls_handler.unregister_stream(application, stream);
    };
    source_hooks.set_renditions = [&hls_handler](const std::string& application,
                                                 const std::string& master,
                                                 std::vector<rtmp_server::hls::Rendition> renditions) {
        hls_handler.set_renditions(application, master, std::move(renditions));
    };
    rtmp_server::transcoding::native::SourceJobManager source_job_manager(std::move(source_hooks),
                                                                          "/hls");

    const auto source_job_json = [&hls_handler](const rtmp_server::transcoding::native::SourceJobSnapshot& job) {
        auto esc = [](std::string_view v) {
            std::string out;
            for (char c : v) {
                if (c == '"' || c == '\\') out.push_back('\\');
                out.push_back(c);
            }
            return out;
        };
        // Source-transcode renditions never register with LiveFanout/StreamManager
        // (no RTMP subscriber exists for a pull-based job's own output), so this
        // is the only place live bandwidth/viewer numbers for the job exist —
        // derived from actual HLS playlist/segment request traffic, summed
        // across every rendition (see HlsHttpHandler::aggregate_link_stats).
        const auto link_stats = hls_handler.aggregate_link_stats(job.application, job.name);
        std::ostringstream os;
        os << R"({"id":")" << esc(job.application + ":" + job.name) << R"(","application":")"
           << esc(job.application) << R"(","name":")" << esc(job.name) << R"(","source_url":")"
           << esc(job.source_url) << R"(","template_name":")" << esc(job.template_name)
           << R"(","master_hls_path":")" << esc(job.master_hls_path) << R"(","status":")"
           << esc(job.status) << R"(","detail":")" << esc(job.detail) << R"(","enabled":)"
           << (job.enabled ? "true" : "false") << R"(,"auto_restart":)"
           << (job.auto_restart ? "true" : "false") << R"(,"restart_delay_seconds":)"
           << job.restart_delay_seconds << R"(,"bytes_total":)" << link_stats.bytes_total
           << R"(,"viewer_count":)" << link_stats.viewer_count << R"(,"outputs":[)";
        for (std::size_t i = 0; i < job.renditions.size(); ++i) {
            const auto& r = job.renditions[i];
            if (i) os << ',';
            os << R"({"name":")" << esc(r.name) << R"(","stream":")" << esc(r.output_stream)
               << R"(","video_codec":"h264","video_bitrate":)" << r.video_bitrate << R"(,"width":)"
               << r.width << R"(,"height":)" << r.height << "}";
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
        [&source_job_manager, &config, source_job_json](
            std::string_view application, std::string_view name, std::string_view source_url,
            std::string_view template_name, std::string_view rules, bool auto_restart,
            std::uint32_t restart_delay_seconds) -> rtmp_server::core::Result<std::string> {
            auto parsed = rtmp_server::transcoding::PresetCatalogue::parse(rules);
            if (!parsed) return parsed.error();
            rtmp_server::transcoding::native::SourceJobConfig cfg;
            cfg.application = std::string(application);
            cfg.name = std::string(name);
            cfg.source_url = std::string(source_url);
            cfg.template_name = std::string(template_name);
            cfg.auto_restart = auto_restart;
            cfg.restart_delay_seconds = restart_delay_seconds;
            for (const auto& rule : parsed.value().rules()) {
                for (const auto& preset : rule.presets) {
                    using rtmp_server::transcoding::AudioCodec;
                    using rtmp_server::transcoding::BackendKind;
                    using rtmp_server::transcoding::VideoCodec;
                    const bool video_ok = preset.video_codec == VideoCodec::H264 ||
                                          preset.video_codec == VideoCodec::Passthrough ||
                                          preset.video_codec == VideoCodec::Disabled;
                    const bool audio_ok = preset.audio_codec == AudioCodec::Aac ||
                                          preset.audio_codec == AudioCodec::Passthrough ||
                                          preset.audio_codec == AudioCodec::Disabled;
                    if (!video_ok || !audio_ok || preset.backend != BackendKind::Software) {
                        return rtmp_server::core::Error(
                            rtmp_server::core::ErrorCode::InvalidConfiguration,
                            rtmp_server::core::ErrorCategory::Configuration,
                            "source transcode supports H.264 + AAC on the software backend only");
                    }
                    rtmp_server::transcoding::native::RenditionSpec spec;
                    spec.name = preset.name;
                    spec.output_stream = preset.outgoing_stream_name;
                    spec.width = preset.width.value_or(0);
                    spec.height = preset.height.value_or(0);
                    spec.video_bitrate = static_cast<std::uint32_t>(preset.video_bitrate);
                    spec.gop = preset.keyframe_interval.value_or(60);
                    spec.audio_bitrate = static_cast<std::uint32_t>(preset.audio_bitrate);
                    spec.fit_mode = preset.fit_mode;
                    cfg.renditions.push_back(std::move(spec));
                }
            }
            if (cfg.renditions.size() > config.transcoding_max_outputs_per_job) {
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
        });
#endif

    management_api.set_live_state_provider([&stream_manager, &stream_registry, &stream_id_registry, &pool] {
        std::vector<rtmp_server::management::LiveState> states;
        const auto registrations = stream_registry.snapshot();
        for (const auto& application : stream_manager.list_applications()) {
            for (const auto& stream : stream_manager.list_streams(application.name)) {
                rtmp_server::management::LiveState state;
                state.application = stream.application;
                state.name = stream.name;
                state.is_live = std::ranges::any_of(registrations, [&stream](const auto& registration) {
                    return registration.app == stream.application && registration.stream_key == stream.name;
                });
                if (state.is_live) {
                    if (auto id = stream_id_registry.find(stream.application, stream.name)) {
                        state.viewer_count = pool.subscriber_count(*id);
                    }
                }
                states.push_back(std::move(state));
            }
        }
        return states;
    });

    hls_handler.set_next([&management_api](const rtmp_server::control::HttpRequest& request) {
        return management_api.handle(request);
    });
    // Caddy drains these loopback responses and performs the public
    // asynchronous client I/O. HLS players request a newly published segment
    // in synchronized bursts, so size the blocking loopback pool from the
    // machine rather than imposing the old fixed 16-request bottleneck.
    const auto hardware_threads = std::max(1U, std::thread::hardware_concurrency());
    const auto hls_http_workers =
        std::clamp<std::size_t>(static_cast<std::size_t>(hardware_threads) * 16, 64, 256);
    rtmp_server::control::HttpServer api_server(
        {config.api_bind_address, config.api_port, 65'535, hls_http_workers, 65'536,
         16 * 1024, 128 * 1024});
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
    if (transcoder) transcoder->stop();
    api_server.stop();

    if (!run_result) {
        std::fprintf(stderr, "worker pool exited with error: %s\n",
                      run_result.error().message().c_str());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
