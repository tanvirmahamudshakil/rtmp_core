#include "rtmp_server/transcoding/native/source_job_manager.hpp"

#include <algorithm>
#include <limits>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error job_error(std::string message) {
    return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Configuration,
                       std::move(message));
}

core::Result<void> validate_source_url(std::string_view url) {
    if (url.starts_with("rtmp://") || url.starts_with("http://") || url.starts_with("https://")) {
        return {};
    }
    return job_error("source URL must use rtmp://, http:// or https://");
}

std::string key_of(const std::string& application, const std::string& name) {
    return application + "/" + name;
}

std::uint64_t peak_hls_bandwidth(std::uint64_t average) {
    // BANDWIDTH is peak aggregate bitrate, not just the encoder target.
    // Reserve room for MPEG-TS/PES headers, keyframe bursts and rate-control
    // variation so ABR clients do not select a rendition they cannot sustain.
    constexpr std::uint64_t kPeakPercent = 125;
    if (average > std::numeric_limits<std::uint64_t>::max() / kPeakPercent) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (average * kPeakPercent + 99) / 100;
}

} // namespace

// The management API only ever hands source jobs H.264/AAC renditions on the
// software backend; RenditionSpec itself carries no codec/backend fields, so
// that pairing is implicit, same as it was in the deleted ffmpeg-spawning
// manager.
core::Result<std::vector<RenditionSpec>> parse_source_job_renditions(std::string_view rules) {
    auto parsed = PresetCatalogue::parse(rules);
    if (!parsed) return parsed.error();

    std::vector<RenditionSpec> renditions;
    for (const auto& rule : parsed.value().rules()) {
        for (const auto& preset : rule.presets) {
            const bool video_ok = preset.video_codec == VideoCodec::H264 ||
                                  preset.video_codec == VideoCodec::Passthrough ||
                                  preset.video_codec == VideoCodec::Disabled;
            const bool audio_ok = preset.audio_codec == AudioCodec::Aac ||
                                  preset.audio_codec == AudioCodec::Passthrough ||
                                  preset.audio_codec == AudioCodec::Disabled;
            if (!video_ok || !audio_ok || preset.backend != BackendKind::Software) {
                return job_error("source transcode supports H.264 + AAC on the software backend only");
            }
            RenditionSpec spec;
            spec.name = preset.name;
            spec.output_stream = preset.outgoing_stream_name;
            spec.width = preset.width.value_or(0);
            spec.height = preset.height.value_or(0);
            spec.video_bitrate = static_cast<std::uint32_t>(preset.video_bitrate);
            spec.gop = preset.keyframe_interval.value_or(60);
            spec.audio_bitrate = static_cast<std::uint32_t>(preset.audio_bitrate);
            spec.fit_mode = preset.fit_mode;
            renditions.push_back(std::move(spec));
        }
    }
    return renditions;
}

SourceJobManager::SourceJobManager(Hooks hooks, persistence::Store* store, Options options,
                                   std::string hls_route_prefix)
    : hooks_(std::move(hooks)),
      store_(store),
      options_(options),
      route_prefix_(std::move(hls_route_prefix)) {
    monitor_running_.store(true);
    monitor_thread_ = std::thread([this] { monitor_loop(); });
}

SourceJobManager::~SourceJobManager() {
    if (monitor_running_.exchange(false)) {
        monitor_wake_cv_.notify_all();
    }
    if (monitor_thread_.joinable()) monitor_thread_.join();
    stop_all();
}

std::string SourceJobManager::master_path(const std::string& application,
                                          const std::string& name) const {
    return route_prefix_ + "/" + application + "/" + name + "/master.m3u8";
}

void SourceJobManager::publish_master_locked(const SourceJobConfig& config,
                                             const std::vector<PullerRendition>& renditions) {
    std::vector<hls::Rendition> master_renditions;
    master_renditions.reserve(renditions.size());
    const auto fps = std::max<std::uint32_t>(config.fps, 1);
    for (const auto& pr : renditions) {
        const auto& spec = pr.spec;
        hls::Rendition rendition;
        rendition.uri = "../" + spec.output_stream + "/index.m3u8";
        rendition.average_bandwidth = spec.video_bitrate + spec.audio_bitrate;
        rendition.bandwidth = peak_hls_bandwidth(rendition.average_bandwidth);
        rendition.codecs = "avc1.64001f,mp4a.40.2";
        rendition.width = spec.width;
        rendition.height = spec.height;
        rendition.frame_rate = static_cast<double>(fps);
        rendition.name = spec.name;
        master_renditions.push_back(std::move(rendition));
    }
    if (hooks_.set_renditions) {
        hooks_.set_renditions(config.application, config.name, std::move(master_renditions));
    }
}

void SourceJobManager::build_renditions_locked(Job& job) {
    job.renditions.clear();
    job.renditions.reserve(job.config.renditions.size());
    for (const auto& spec : job.config.renditions) {
        hls::SegmentStoreConfig store_config;
        store_config.live_window_segments = options_.live_window_segments;
        store_config.retention_grace_segments = options_.retention_grace_segments;
        store_config.max_total_bytes = options_.max_total_bytes_per_rendition;
        store_config.target_duration_seconds = options_.target_duration_seconds;
        PullerRendition pr;
        pr.spec = spec;
        pr.store = std::make_shared<hls::SegmentStore>(store_config);
        job.renditions.push_back(std::move(pr));
    }
}

void SourceJobManager::register_outputs_locked(const Job& job) {
    if (!hooks_.register_output) return;
    for (const auto& pr : job.renditions) {
        hooks_.register_output(job.config.application, pr.spec.output_stream, pr.store);
    }
}

void SourceJobManager::unregister_outputs_locked(const Job& job) {
    if (!hooks_.unregister_output) return;
    for (const auto& pr : job.renditions) {
        hooks_.unregister_output(job.config.application, pr.spec.output_stream);
    }
}

void SourceJobManager::start_locked(Job& job) {
    build_renditions_locked(job);
    register_outputs_locked(job);
    publish_master_locked(job.config, job.renditions);

    // HlsSourcePuller owns its renditions by value (copies the shared_ptr
    // stores), so a fresh puller is constructed on every (re)start.
    job.puller = std::make_unique<HlsSourcePuller>(job.config.source_url, job.renditions, job.config.fps);
    job.puller->start();
    job.enabled = true;
    job.detail_override.clear();
    job.restart_scheduled = false;
}

void SourceJobManager::teardown_locked(Job& job) {
    if (job.puller) {
        job.puller->stop();
        job.puller.reset();
    }
    unregister_outputs_locked(job);
}

SourceJobSnapshot SourceJobManager::snapshot_locked(const Job& job) const {
    SourceJobSnapshot snapshot;
    snapshot.application = job.config.application;
    snapshot.name = job.config.name;
    snapshot.source_url = job.config.source_url;
    snapshot.template_name = job.config.template_name;
    snapshot.master_hls_path = master_path(job.config.application, job.config.name);
    snapshot.enabled = job.enabled;
    snapshot.auto_restart = job.config.auto_restart;
    snapshot.restart_delay_seconds = job.config.restart_delay_seconds;
    snapshot.renditions = job.config.renditions;
    if (!job.enabled) {
        snapshot.status = "disabled";
        snapshot.detail = job.detail_override;
        return snapshot;
    }
    if (!job.detail_override.empty() && !job.puller) {
        snapshot.status = "error";
        snapshot.detail = job.detail_override;
        return snapshot;
    }
    if (!job.puller) {
        snapshot.status = "stopped";
        return snapshot;
    }
    switch (job.puller->status()) {
        case PullerStatus::Starting: snapshot.status = "starting"; break;
        case PullerStatus::Running: snapshot.status = "running"; break;
        case PullerStatus::Error: snapshot.status = "error"; break;
        case PullerStatus::Stopped: snapshot.status = "stopped"; break;
    }
    snapshot.detail = job.puller->detail();
    return snapshot;
}

void SourceJobManager::persist_locked(const Job& job) {
    if (store_ == nullptr) return;
    persistence::SourceJobRow row;
    row.application = job.config.application;
    row.name = job.config.name;
    row.source_url = job.config.source_url;
    row.template_name = job.config.template_name;
    row.rules = job.config.rules;
    row.auto_restart = job.config.auto_restart;
    row.restart_delay_seconds = job.config.restart_delay_seconds;
    row.enabled = job.enabled;
    (void)store_->upsert_source_job(row);
}

void SourceJobManager::load_from_store() {
    if (store_ == nullptr) return;
    auto rows = store_->load_source_jobs();
    if (!rows) return;
    for (const auto& row : rows.value()) {
        auto renditions = parse_source_job_renditions(row.rules);
        std::lock_guard lock(mutex_);
        const std::string key = key_of(row.application, row.name);

        Job job;
        job.config.application = row.application;
        job.config.name = row.name;
        job.config.source_url = row.source_url;
        job.config.template_name = row.template_name;
        job.config.rules = row.rules;
        job.config.auto_restart = row.auto_restart;
        job.config.restart_delay_seconds = row.restart_delay_seconds;
        if (!renditions) {
            job.enabled = false;
            job.detail_override = "failed to reload persisted job: " + renditions.error().message();
            jobs_.emplace(key, std::move(job));
            continue;
        }
        job.config.renditions = std::move(renditions).value();
        if (row.enabled) {
            start_locked(job);
        } else {
            job.enabled = false;
        }
        jobs_.emplace(key, std::move(job));
    }
}

void SourceJobManager::monitor_loop() {
    // Polls every job's puller roughly 5x/s and respawns a job whose
    // restart_at has passed (scheduled below when a puller reports Error and
    // auto_restart is set).
    while (monitor_running_.load()) {
        {
            std::unique_lock lock(monitor_wake_mutex_);
            monitor_wake_cv_.wait_for(lock, std::chrono::milliseconds(200),
                                      [this] { return !monitor_running_.load(); });
        }
        if (!monitor_running_.load()) break;

        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto& [key, job] : jobs_) {
            (void)key;
            if (!job.enabled || !job.config.auto_restart) continue;
            if (job.puller && job.puller->status() == PullerStatus::Error && !job.restart_scheduled) {
                job.restart_scheduled = true;
                job.restart_at = now + std::chrono::seconds(job.config.restart_delay_seconds);
                continue;
            }
            if (job.restart_scheduled && now >= job.restart_at &&
                job.restart_attempts <= options_.max_restart_attempts) {
                ++job.restart_attempts;
                teardown_locked(job);
                start_locked(job);
            }
        }
    }
}

core::Result<SourceJobSnapshot> SourceJobManager::create(const SourceJobConfig& config) {
    if (config.name.empty()) return job_error("source job requires an output name");
    if (config.source_url.empty()) return job_error("source job requires a source URL");
    if (auto valid = validate_source_url(config.source_url); !valid) return valid.error();
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
    start_locked(job);
    auto snapshot = snapshot_locked(job);
    persist_locked(job);

    jobs_.emplace(key, std::move(job));
    return snapshot;
}

core::Result<SourceJobSnapshot> SourceJobManager::set_enabled(const std::string& application,
                                                              const std::string& name, bool enabled) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(key_of(application, name));
    if (it == jobs_.end()) return job_error("no such source job");
    Job& job = it->second;
    if (job.enabled == enabled) return snapshot_locked(job);

    if (enabled) {
        job.restart_attempts = 0;
        start_locked(job); // respawn puller from stored config
    } else {
        teardown_locked(job); // stop transcoding; source's HLS output stops updating
        job.enabled = false;
    }
    persist_locked(job);
    return snapshot_locked(job);
}

core::Result<SourceJobSnapshot> SourceJobManager::restart(const std::string& application,
                                                          const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(key_of(application, name));
    if (it == jobs_.end()) return job_error("no such source job");
    Job& job = it->second;
    if (!job.enabled) return job_error("job is disabled; enable it first");
    teardown_locked(job);
    job.restart_attempts = 0;
    start_locked(job);
    return snapshot_locked(job);
}

bool SourceJobManager::remove(const std::string& application, const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(key_of(application, name));
    if (it == jobs_.end()) return false;
    teardown_locked(it->second);
    if (store_ != nullptr) (void)store_->delete_source_job(application, name);
    jobs_.erase(it);
    return true;
}

std::vector<SourceJobSnapshot> SourceJobManager::list(const std::string& application) const {
    std::lock_guard lock(mutex_);
    std::vector<SourceJobSnapshot> result;
    for (const auto& [key, job] : jobs_) {
        (void)key;
        if (!application.empty() && job.config.application != application) continue;
        result.push_back(snapshot_locked(job));
    }
    return result;
}

void SourceJobManager::stop_all() {
    std::lock_guard lock(mutex_);
    for (auto& [key, job] : jobs_) {
        (void)key;
        teardown_locked(job);
    }
}

} // namespace rtmp_server::transcoding::native
