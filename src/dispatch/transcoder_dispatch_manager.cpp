#include "rtmp_server/dispatch/transcoder_dispatch_manager.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/transcoding/native/codec_tags.hpp"

namespace rtmp_server::dispatch {
namespace {

core::Error job_error(core::ErrorCode code, std::string message) {
    return core::Error(code, core::ErrorCategory::Configuration, std::move(message));
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

// Same 25% peak-over-average reservation the source-job and ingest-transcode
// ladders declare for BANDWIDTH, kept here rather than shared: this manager
// must not depend on the codec-gated headers those live in.
std::uint64_t peak_hls_bandwidth(std::uint64_t average) {
    constexpr std::uint64_t kPeakPercent = 125;
    if (average > std::numeric_limits<std::uint64_t>::max() / kPeakPercent) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (average * kPeakPercent + 99) / 100;
}

// Parses exactly the shape renditions_to_json produces -- a flat JSON array
// of flat objects with a fixed field set, in field order, no nesting, no
// escaping in values (every value here is either a plain identifier-ish
// string or a number). This is not a general JSON parser and must never be
// pointed at arbitrary/untrusted input; it exists only to round-trip this
// manager's own persisted rows.
core::Result<std::vector<DispatchedRendition>> renditions_from_json(std::string_view json) {
    std::vector<DispatchedRendition> renditions;
    std::size_t pos = 0;
    while (true) {
        const auto object_start = json.find('{', pos);
        if (object_start == std::string_view::npos) break;
        const auto object_end = json.find('}', object_start);
        if (object_end == std::string_view::npos) {
            return job_error(core::ErrorCode::InvalidConfiguration, "unterminated rendition object");
        }
        const auto object = json.substr(object_start, object_end - object_start + 1);

        const auto string_field = [&](std::string_view field) -> std::string {
            const auto key = "\"" + std::string(field) + "\":\"";
            const auto at = object.find(key);
            if (at == std::string_view::npos) return {};
            const auto value_start = at + key.size();
            const auto value_end = object.find('"', value_start);
            if (value_end == std::string_view::npos) return {};
            return std::string(object.substr(value_start, value_end - value_start));
        };
        const auto number_field = [&](std::string_view field) -> std::uint32_t {
            const auto key = "\"" + std::string(field) + "\":";
            const auto at = object.find(key);
            if (at == std::string_view::npos) return 0;
            const auto value_start = at + key.size();
            std::uint32_t value = 0;
            for (auto i = value_start; i < object.size() && object[i] >= '0' && object[i] <= '9'; ++i) {
                value = value * 10 + static_cast<std::uint32_t>(object[i] - '0');
            }
            return value;
        };

        DispatchedRendition rendition;
        rendition.name = string_field("name");
        rendition.output_stream = string_field("output_stream");
        rendition.width = number_field("width");
        rendition.height = number_field("height");
        rendition.video_bitrate = number_field("video_bitrate");
        rendition.audio_bitrate = number_field("audio_bitrate");
        if (rendition.output_stream.empty()) {
            return job_error(core::ErrorCode::InvalidConfiguration,
                             "stored rendition has no output_stream");
        }
        renditions.push_back(std::move(rendition));
        pos = object_end + 1;
    }
    if (renditions.empty()) {
        return job_error(core::ErrorCode::InvalidConfiguration, "no renditions found in stored JSON");
    }
    return renditions;
}

std::string renditions_to_json(const std::vector<DispatchedRendition>& renditions) {
    std::ostringstream os;
    os << '[';
    for (std::size_t i = 0; i < renditions.size(); ++i) {
        const auto& r = renditions[i];
        if (i) os << ',';
        os << R"({"name":")" << json_escape(r.name) << R"(","output_stream":")"
           << json_escape(r.output_stream) << R"(","width":)" << r.width << R"(,"height":)"
           << r.height << R"(,"video_bitrate":)" << r.video_bitrate << R"(,"audio_bitrate":)"
           << r.audio_bitrate << '}';
    }
    os << ']';
    return os.str();
}

} // namespace

TranscoderDispatchManager::TranscoderDispatchManager(cluster::NodeRegistry* node_registry,
                                                     persistence::Store* store, Hooks hooks,
                                                     Options options)
    : node_registry_(node_registry), store_(store), hooks_(std::move(hooks)), options_(options),
      http_(options.http) {}

TranscoderDispatchManager::~TranscoderDispatchManager() = default;

std::string TranscoderDispatchManager::key_of(std::string_view application, std::string_view name) {
    return std::string(application) + "/" + std::string(name);
}

void TranscoderDispatchManager::declare_master_locked(const Job& job) {
    if (!hooks_.set_renditions) return;
    std::vector<hls::Rendition> master;
    master.reserve(job.config.renditions.size());
    const auto fps = std::max<std::uint32_t>(job.config.fps, 1);
    for (const auto& r : job.config.renditions) {
        hls::Rendition entry;
        entry.uri = "../" + r.output_stream + "/index.m3u8";
        entry.average_bandwidth = r.video_bitrate + r.audio_bitrate;
        entry.bandwidth = peak_hls_bandwidth(entry.average_bandwidth);
        entry.codecs =
            transcoding::native::hls_codecs_attribute(r.width, r.height, fps, r.audio_bitrate);
        entry.width = r.width;
        entry.height = r.height;
        entry.frame_rate = static_cast<double>(fps);
        entry.name = r.name;
        master.push_back(std::move(entry));
    }
    hooks_.set_renditions(job.config.application, job.config.name, std::move(master));
}

void TranscoderDispatchManager::persist_locked(const Job& job) {
    if (store_ == nullptr) return;
    persistence::TranscoderJobRow row;
    row.application = job.config.application;
    row.name = job.config.name;
    row.source_url = job.config.source_url;
    row.fps = job.config.fps;
    row.renditions_json = renditions_to_json(job.config.renditions);
    if (auto saved = store_->upsert_transcoder_job(row); !saved) {
        RTMP_LOG(observability::LogLevel::Warn, "transcoder-dispatch", "persist failed",
                 {{"application", job.config.application},
                  {"name", job.config.name},
                  {"error", saved.error().message()}});
    }
}

core::Result<void> TranscoderDispatchManager::dispatch_locked(Job& job,
                                                              const cluster::NodeStatus& node) {
    std::ostringstream body;
    body << R"({"id":")" << json_escape(key_of(job.config.application, job.config.name))
         << R"(","source_url":")" << json_escape(job.config.source_url) << R"(","fps":)"
         << job.config.fps << R"(,"target_application":")" << json_escape(job.config.application)
         << R"(","origin_rtmp_host":")" << json_escape(options_.origin_rtmp_host)
         << R"(","origin_rtmp_port":)" << options_.origin_rtmp_port << R"(,"renditions":)"
         << renditions_to_json(job.config.renditions) << '}';

    auto response = http_.post(node.address, options_.agent_port, "/jobs", body.str());
    if (!response) {
        job.state = TranscoderJobState::Unassignable;
        job.detail = "agent unreachable: " + response.error().message();
        return response.error();
    }
    if (response.value().status / 100 != 2) {
        job.state = TranscoderJobState::Unassignable;
        job.detail = "agent rejected the job (HTTP " + std::to_string(response.value().status) +
                    "): " + response.value().body;
        return job_error(core::ErrorCode::Conflict, job.detail);
    }

    job.assigned_node_id = node.id;
    job.assigned_node_address = node.address;
    job.state = TranscoderJobState::Assigning;
    job.detail = "assigned to " + node.id + "; waiting for renditions to publish";
    return {};
}

core::Result<TranscoderJobStatus> TranscoderDispatchManager::create(const TranscoderJobConfig& config,
                                                                   std::int64_t now_unix) {
    if (config.application.empty() || config.name.empty()) {
        return job_error(core::ErrorCode::InvalidConfiguration, "application and name are required");
    }
    if (config.source_url.empty()) {
        return job_error(core::ErrorCode::InvalidConfiguration, "source_url is required");
    }
    if (config.renditions.empty()) {
        return job_error(core::ErrorCode::InvalidConfiguration, "at least one rendition is required");
    }
    for (const auto& r : config.renditions) {
        if (r.output_stream.empty()) {
            return job_error(core::ErrorCode::InvalidConfiguration,
                             "every rendition needs an output_stream");
        }
        if (r.output_stream == config.name) {
            return job_error(core::ErrorCode::InvalidConfiguration,
                             "a rendition may not reuse the job name");
        }
    }
    for (std::size_t i = 0; i < config.renditions.size(); ++i) {
        for (std::size_t j = i + 1; j < config.renditions.size(); ++j) {
            if (config.renditions[i].output_stream == config.renditions[j].output_stream) {
                return job_error(core::ErrorCode::InvalidConfiguration,
                                 "two renditions share one output_stream");
            }
        }
    }

    Job job;
    job.config = config;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = key_of(config.application, config.name);
    if (jobs_.contains(key)) {
        return job_error(core::ErrorCode::Conflict, "a job with this application/name already exists");
    }

    const auto node = node_registry_ != nullptr
                          ? node_registry_->least_loaded(cluster::NodeRole::Transcoder, now_unix)
                          : std::nullopt;
    if (node) {
        // A dispatch failure (agent unreachable/rejected) still leaves the job
        // recorded as Unassignable rather than failing create() outright --
        // retry_unassigned() will place it once a transcoder is reachable,
        // the same "never give up" posture SourceJobManager takes on its own
        // pull failures.
        (void)dispatch_locked(job, *node);
    } else {
        job.state = TranscoderJobState::Unassignable;
        job.detail = "no healthy transcoder node available";
    }

    for (const auto& r : job.config.renditions) {
        rendition_index_[key_of(job.config.application, r.output_stream)] = key;
    }
    declare_master_locked(job);
    persist_locked(job);

    TranscoderJobStatus status;
    status.application = job.config.application;
    status.name = job.config.name;
    status.source_url = job.config.source_url;
    status.assigned_node_id = job.assigned_node_id.value_or("");
    status.state = job.state;
    status.detail = job.detail;
    status.renditions = job.config.renditions;
    jobs_[key] = std::move(job);
    return status;
}

core::Result<void> TranscoderDispatchManager::remove(std::string_view application,
                                                     std::string_view name) {
    std::string node_address;
    std::string job_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = key_of(application, name);
        const auto job = jobs_.find(key);
        if (job == jobs_.end()) return job_error(core::ErrorCode::NotFound, "no such transcoder job");
        for (const auto& r : job->second.config.renditions) {
            rendition_index_.erase(key_of(application, r.output_stream));
        }
        node_address = job->second.assigned_node_address;
        job_id = key;
        if (hooks_.unset_renditions) hooks_.unset_renditions(std::string(application), std::string(name));
        jobs_.erase(job);
    }
    if (store_ != nullptr) {
        if (auto deleted = store_->delete_transcoder_job(application, name); !deleted) {
            return deleted.error();
        }
    }
    if (!node_address.empty()) {
        // Best-effort: tell the agent to stop pulling/pushing. A failure here
        // does not block removal -- the origin refuses those renditions'
        // publishes from now on regardless (is_expected_publish no longer
        // matches), so a node that never received this simply has its pushes
        // rejected on arrival instead of the job being cleaned up twice.
        auto stopped = http_.del(node_address, options_.agent_port, "/jobs/" + job_id);
        if (!stopped) {
            RTMP_LOG(observability::LogLevel::Warn, "transcoder-dispatch", "agent stop-job call failed",
                     {{"application", std::string(application)},
                      {"name", std::string(name)},
                      {"error", stopped.error().message()}});
        }
    }
    return {};
}

std::vector<TranscoderJobStatus> TranscoderDispatchManager::list(std::string_view application) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TranscoderJobStatus> result;
    for (const auto& [key, job] : jobs_) {
        if (!application.empty() && job.config.application != application) continue;
        TranscoderJobStatus status;
        status.application = job.config.application;
        status.name = job.config.name;
        status.source_url = job.config.source_url;
        status.assigned_node_id = job.assigned_node_id.value_or("");
        status.state = job.state;
        status.detail = job.detail;
        status.renditions = job.config.renditions;
        result.push_back(std::move(status));
    }
    std::ranges::sort(result, [](const TranscoderJobStatus& a, const TranscoderJobStatus& b) {
        return std::tie(a.application, a.name) < std::tie(b.application, b.name);
    });
    return result;
}

bool TranscoderDispatchManager::is_expected_publish(std::string_view application,
                                                    std::string_view stream) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rendition_index_.contains(key_of(application, stream));
}

void TranscoderDispatchManager::note_publish_state(std::string_view application,
                                                   std::string_view stream, bool live) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto index = rendition_index_.find(key_of(application, stream));
    if (index == rendition_index_.end()) return;
    const auto job = jobs_.find(index->second);
    if (job == jobs_.end()) return;

    if (live) {
        job->second.live_renditions.insert(std::string(stream));
        if (job->second.state == TranscoderJobState::Assigning ||
            job->second.state == TranscoderJobState::Lost) {
            job->second.state = TranscoderJobState::Running;
            job->second.detail = "running on " + job->second.assigned_node_id.value_or("?");
        }
        return;
    }

    job->second.live_renditions.erase(std::string(stream));
    if (job->second.state == TranscoderJobState::Running && job->second.live_renditions.empty()) {
        job->second.state = TranscoderJobState::Lost;
        job->second.detail = "every rendition stopped publishing";
    }
}

void TranscoderDispatchManager::retry_unassigned(std::int64_t now_unix) {
    if (node_registry_ == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, job] : jobs_) {
        if (job.state != TranscoderJobState::Unassignable && job.state != TranscoderJobState::Lost) {
            continue;
        }
        const auto node = node_registry_->least_loaded(cluster::NodeRole::Transcoder, now_unix);
        if (!node) continue;
        (void)dispatch_locked(job, *node);
        persist_locked(job);
    }
}

void TranscoderDispatchManager::load_from_store() {
    if (store_ == nullptr) return;
    auto rows = store_->load_transcoder_jobs();
    if (!rows) {
        RTMP_LOG(observability::LogLevel::Warn, "transcoder-dispatch", "job load failed",
                 {{"error", rows.error().message()}});
        return;
    }

    // Placement is deliberately NOT attempted here: this runs during startup,
    // before this origin has heard a single heartbeat from any transcoder
    // node, so every job would be marked Unassignable for no real reason.
    // retry_unassigned(), called from the same periodic tick the origin's own
    // heartbeat already runs on, places each one the moment a transcoder
    // reports in.
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& row : rows.value()) {
        auto renditions = renditions_from_json(row.renditions_json);
        if (!renditions) {
            RTMP_LOG(observability::LogLevel::Warn, "transcoder-dispatch", "job skipped",
                     {{"application", row.application},
                      {"name", row.name},
                      {"error", renditions.error().message()}});
            continue;
        }
        Job job;
        job.config.application = row.application;
        job.config.name = row.name;
        job.config.source_url = row.source_url;
        job.config.fps = row.fps;
        job.config.renditions = std::move(renditions).value();
        job.state = TranscoderJobState::Unassignable;
        job.detail = "restored after a control-plane restart; awaiting a transcoder node";

        const auto key = key_of(row.application, row.name);
        for (const auto& r : job.config.renditions) {
            rendition_index_[key_of(row.application, r.output_stream)] = key;
        }
        declare_master_locked(job);
        jobs_[key] = std::move(job);
    }
}

} // namespace rtmp_server::dispatch
