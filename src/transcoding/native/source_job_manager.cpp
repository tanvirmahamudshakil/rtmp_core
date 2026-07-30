#include "rtmp_server/transcoding/native/source_job_manager.hpp"

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error job_error(std::string message) {
    return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Configuration,
                       std::move(message));
}

std::string status_text(PullerStatus status) {
    switch (status) {
        case PullerStatus::Starting: return "starting";
        case PullerStatus::Running: return "running";
        case PullerStatus::Error: return "error";
        case PullerStatus::Stopped: return "stopped";
    }
    return "unknown";
}

std::string key_of(const std::string& application, const std::string& name) {
    return application + "/" + name;
}

} // namespace

SourceJobManager::SourceJobManager(Hooks hooks, std::string hls_route_prefix)
    : hooks_(std::move(hooks)), route_prefix_(std::move(hls_route_prefix)) {}

SourceJobManager::~SourceJobManager() { stop_all(); }

std::string SourceJobManager::master_path(const std::string& application,
                                          const std::string& name) const {
    return route_prefix_ + "/" + application + "/" + name + "/master.m3u8";
}

void SourceJobManager::teardown_locked(Job& job) {
    if (job.puller) job.puller->stop();
    if (hooks_.unregister_store) {
        for (const auto& stream : job.output_streams) {
            hooks_.unregister_store(job.config.application, stream);
        }
    }
}

core::Result<SourceJobSnapshot> SourceJobManager::create(const SourceJobConfig& config) {
    if (config.name.empty()) return job_error("source job requires an output name");
    if (config.source_url.empty()) return job_error("source job requires a source URL");
    if (config.renditions.empty()) return job_error("source job requires at least one rendition");
    for (const auto& rendition : config.renditions) {
        if (rendition.output_stream.empty()) return job_error("every rendition needs an output stream name");
    }

    std::lock_guard lock(mutex_);
    const std::string key = key_of(config.application, config.name);
    if (auto it = jobs_.find(key); it != jobs_.end()) {
        teardown_locked(it->second); // replace an existing job with the same name
        jobs_.erase(it);
    }

    Job job;
    job.config = config;

    std::vector<PullerRendition> puller_renditions;
    std::vector<hls::Rendition> master_renditions;
    puller_renditions.reserve(config.renditions.size());
    for (const auto& spec : config.renditions) {
        auto store = std::make_shared<hls::SegmentStore>();
        if (hooks_.register_store) hooks_.register_store(config.application, spec.output_stream, store);
        job.output_streams.push_back(spec.output_stream);

        hls::Rendition rendition;
        rendition.uri = "../" + spec.output_stream + "/index.m3u8";
        rendition.bandwidth = spec.video_bitrate + spec.audio_bitrate;
        rendition.width = spec.width;
        rendition.height = spec.height;
        master_renditions.push_back(std::move(rendition));

        puller_renditions.push_back(PullerRendition{spec, std::move(store)});
    }

    if (hooks_.set_renditions) {
        hooks_.set_renditions(config.application, config.name, std::move(master_renditions));
    }

    job.puller = std::make_unique<HlsSourcePuller>(config.source_url, std::move(puller_renditions),
                                                   config.fps);
    job.puller->start();

    SourceJobSnapshot snapshot;
    snapshot.application = config.application;
    snapshot.name = config.name;
    snapshot.source_url = config.source_url;
    snapshot.template_name = config.template_name;
    snapshot.master_hls_path = master_path(config.application, config.name);
    snapshot.status = status_text(job.puller->status());
    snapshot.detail = job.puller->detail();
    snapshot.renditions = config.renditions;

    jobs_.emplace(key, std::move(job));
    return snapshot;
}

bool SourceJobManager::remove(const std::string& application, const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(key_of(application, name));
    if (it == jobs_.end()) return false;
    teardown_locked(it->second);
    jobs_.erase(it);
    return true;
}

std::vector<SourceJobSnapshot> SourceJobManager::list(const std::string& application) const {
    std::lock_guard lock(mutex_);
    std::vector<SourceJobSnapshot> result;
    for (const auto& [key, job] : jobs_) {
        if (!application.empty() && job.config.application != application) continue;
        SourceJobSnapshot snapshot;
        snapshot.application = job.config.application;
        snapshot.name = job.config.name;
        snapshot.source_url = job.config.source_url;
        snapshot.template_name = job.config.template_name;
        snapshot.master_hls_path = master_path(job.config.application, job.config.name);
        snapshot.status = job.puller ? status_text(job.puller->status()) : "stopped";
        snapshot.detail = job.puller ? job.puller->detail() : "";
        snapshot.renditions = job.config.renditions;
        result.push_back(std::move(snapshot));
    }
    return result;
}

void SourceJobManager::stop_all() {
    std::lock_guard lock(mutex_);
    for (auto& [key, job] : jobs_) teardown_locked(job);
    jobs_.clear();
}

} // namespace rtmp_server::transcoding::native
