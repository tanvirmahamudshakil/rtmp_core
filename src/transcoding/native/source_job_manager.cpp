#include "rtmp_server/transcoding/native/source_job_manager.hpp"

#include <algorithm>
#include <limits>
#include <thread>

#include "rtmp_server/core/cpu_partition.hpp"
#include "rtmp_server/core/error.hpp"
#include "rtmp_server/transcoding/native/codec_tags.hpp"
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
        // Declared from this rung's own geometry and audio bitrate. A fixed
        // string described every rendition as 720p-class High profile with
        // AAC-LC, which mis-describes a 1080p rung and any rung whose audio
        // bitrate puts the encoder into HE-AAC -- and a player is entitled
        // to choose, or refuse, a variant on this attribute alone.
        rendition.codecs = hls_codecs_attribute(spec.width, spec.height, fps, spec.audio_bitrate);
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
    // Segment stores outlive the puller that fills them. A restart (upstream
    // dropped, manual restart, auto-restart) previously threw the stores
    // away and started from an empty live window, so every viewer's playlist
    // went 404/empty until a whole new startup runway had been primed --
    // tens of seconds of dead air for what may have been a two-second
    // upstream hiccup. Reusing them keeps the last window on air while the
    // new pipeline spins up, and lets the puller resume the media sequence
    // instead of colliding with already-cached segment URLs (see
    // HlsSourcePuller::run's `resuming` branch, which was unreachable
    // before this).
    if (job.renditions.size() == job.config.renditions.size()) {
        bool same_outputs = true;
        for (std::size_t i = 0; i < job.renditions.size(); ++i) {
            if (job.renditions[i].store == nullptr ||
                job.renditions[i].spec.output_stream != job.config.renditions[i].output_stream) {
                same_outputs = false;
                break;
            }
        }
        if (same_outputs) {
            for (std::size_t i = 0; i < job.renditions.size(); ++i) {
                job.renditions[i].spec = job.config.renditions[i];
                // A cleanly stopped pipeline marked its store ended
                // (EXT-X-ENDLIST). Reopen it: the window is about to start
                // moving again.
                job.renditions[i].store->mark_live();
            }
            return;
        }
    }

    job.renditions.clear();
    job.renditions.reserve(job.config.renditions.size());
    for (const auto& spec : job.config.renditions) {
        hls::SegmentStoreConfig store_config;
        store_config.live_window_segments = options_.live_window_segments;
        store_config.retention_grace_segments = options_.retention_grace_segments;
        store_config.max_total_bytes = options_.max_total_bytes_per_rendition;
        store_config.target_duration_seconds = options_.target_duration_seconds;
        // A pulled source is not under this server's control: it can stall
        // for a few seconds at any time, and the pipeline is rebuilt around
        // it automatically. Repeating the last complete segment keeps the
        // playlist advancing through that gap so established players hold
        // the stream open instead of ending the session on a frozen
        // playlist. Synthetic copies do not count as real output, so the
        // puller's own stall detection is unaffected (SegmentStoreStats::
        // real_segments_added).
        store_config.repeat_last_segment_on_stall = true;
        // Every rendition is fully re-encoded (SourceTranscoder) onto one
        // re-anchored, monotonic output timeline, so recovery after an outage
        // is seamless -- no EXT-X-DISCONTINUITY, which a flaky upstream would
        // otherwise trigger every minute and freeze players on each one. A
        // real discontinuity from the upstream playlist still propagates.
        store_config.seamless_fallback_recovery = true;
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

std::uint32_t SourceJobManager::cpu_budget_locked() const {
    // When a reservation is configured, jobs divide only the reserved slice
    // (not the whole machine) so their combined thread requests can never
    // spill onto the cores that RTMP ingest/HTTP/admin threads are confined
    // to -- otherwise a busy job could still oversubscribe the box even
    // though it would eventually be pinned away from the other side.
    const auto partition = core::compute_cpu_partition(options_.transcode_cpu_reservation_percent);
    const std::uint32_t cores = !partition.transcode_cores.empty()
                                    ? static_cast<std::uint32_t>(partition.transcode_cores.size())
                                    : (std::thread::hardware_concurrency() > 0
                                           ? std::thread::hardware_concurrency()
                                           : 1);
    std::uint32_t active = 0;
    for (const auto& [key, job] : jobs_) {
        (void)key;
        if (job.enabled) ++active;
    }
    // The caller is about to start a job, which may not be in jobs_ yet.
    active = std::max<std::uint32_t>(active, 1);
    return std::max<std::uint32_t>(1, cores / active);
}

void SourceJobManager::start_locked(Job& job) {
    build_renditions_locked(job);
    register_outputs_locked(job);
    publish_master_locked(job.config, job.renditions);

    // HlsSourcePuller owns its renditions by value (copies the shared_ptr
    // stores), so a fresh puller is constructed on every (re)start.
    // Every running job shares one machine: hand this one its slice rather
    // than letting each job's encoders size themselves from the full core
    // count independently (see SourceTranscoder's constructor). The split is
    // recomputed whenever a job starts, so adding a job narrows the share
    // for jobs started after it and a restart re-levels the rest.
    job.puller = std::make_unique<HlsSourcePuller>(
        job.config.source_url, job.renditions, job.config.fps, cpu_budget_locked(),
        core::compute_cpu_partition(options_.transcode_cpu_reservation_percent).transcode_cores);
    job.puller->start();
    job.enabled = true;
    job.detail_override.clear();
    job.restart_scheduled = false;
    job.running_since.reset();
    // This is the authoritative "a puller is running again" point. Clearing
    // the flag here means an in-flight restart_job that was overtaken (an
    // operator disabled then re-enabled the job while its old puller was
    // being joined) sees the job as already started and does not start a
    // second one on top.
    job.restart_in_progress = false;
}

std::unique_ptr<HlsSourcePuller> SourceJobManager::detach_puller_locked(Job& job) {
    job.running_since.reset();
    return std::move(job.puller);
}

void SourceJobManager::teardown_locked(Job& job, std::unique_ptr<HlsSourcePuller>& retired,
                                       bool release_outputs) {
    retired = detach_puller_locked(job);
    if (release_outputs) unregister_outputs_locked(job);
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

std::chrono::seconds SourceJobManager::restart_delay_for(const SourceJobConfig& config,
                                                         const SourceJobOptions& options,
                                                         std::uint32_t attempts) {
    const std::uint32_t base = std::max<std::uint32_t>(config.restart_delay_seconds, 1);
    const std::uint32_t cap = std::max(options.restart_backoff_cap_seconds, base);
    // Double the wait per consecutive failure, up to the cap. A source that
    // is simply down should be retried patiently rather than hammered every
    // few seconds for hours; a source that dropped once still comes back on
    // the configured delay.
    const std::uint32_t exponent = std::min(attempts, options.max_restart_attempts);
    std::uint64_t delay = base;
    for (std::uint32_t i = 0; i < exponent && delay < cap; ++i) delay *= 2;
    return std::chrono::seconds(static_cast<std::uint32_t>(std::min<std::uint64_t>(delay, cap)));
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

        std::vector<std::string> due;
        {
            std::lock_guard lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            for (auto& [key, job] : jobs_) {
                if (!job.enabled || !job.config.auto_restart || job.restart_in_progress) continue;

                // Forgive the failure streak once this puller has held
                // Running long enough. Without this a job that drops once a
                // day still climbs to the maximum backoff over a week, and
                // (before restarts became unlimited) eventually stopped
                // being restarted at all.
                if (job.puller && job.puller->status() == PullerStatus::Running) {
                    if (!job.running_since) job.running_since = now;
                    if (job.restart_attempts > 0 &&
                        now - *job.running_since >=
                            std::chrono::seconds(options_.healthy_reset_seconds)) {
                        job.restart_attempts = 0;
                    }
                } else if (job.puller && job.puller->status() != PullerStatus::Starting) {
                    job.running_since.reset();
                }

                if (job.puller && job.puller->status() == PullerStatus::Error &&
                    !job.restart_scheduled) {
                    job.restart_scheduled = true;
                    job.restart_at =
                        now + restart_delay_for(job.config, options_, job.restart_attempts);
                    continue;
                }
                // No attempt ceiling: a job is never abandoned permanently.
                // The delay grows to restart_backoff_cap_seconds and stays
                // there, so an upstream that returns after an hour is picked
                // up automatically instead of requiring someone to log in
                // and press restart.
                if (job.restart_scheduled && now >= job.restart_at) {
                    ++job.restart_attempts;
                    due.push_back(key);
                }
            }
        }
        for (const auto& key : due) restart_job(key);
    }
}

void SourceJobManager::restart_job(const std::string& key) {
    // Declared before the lock guard in each scope below so the puller's
    // thread join happens after mutex_ is released.
    std::unique_ptr<HlsSourcePuller> retired;
    {
        std::lock_guard lock(mutex_);
        auto it = jobs_.find(key);
        if (it == jobs_.end()) return;
        Job& job = it->second;
        if (job.restart_in_progress || !job.enabled) return;
        job.restart_in_progress = true;
        // Outputs stay registered across a restart: the retained segment
        // stores keep serving their last window (and, while the source is
        // down, the store's own stall fallback) instead of 404ing every
        // viewer for the length of the rebuild.
        teardown_locked(job, retired, /*release_outputs=*/false);
    }
    retired.reset(); // joins the puller thread with mutex_ released

    std::lock_guard lock(mutex_);
    auto it = jobs_.find(key);
    if (it == jobs_.end()) return; // removed while we were stopping it
    Job& job = it->second;
    if (!job.restart_in_progress) return; // replaced by a fresh create()
    job.restart_in_progress = false;
    if (!job.enabled) return; // disabled while we were stopping it
    start_locked(job);
}

core::Result<SourceJobSnapshot> SourceJobManager::create(const SourceJobConfig& config) {
    if (config.name.empty()) return job_error("source job requires an output name");
    if (config.source_url.empty()) return job_error("source job requires a source URL");
    if (auto valid = validate_source_url(config.source_url); !valid) return valid.error();
    if (config.renditions.empty()) return job_error("source job requires at least one rendition");
    for (const auto& rendition : config.renditions) {
        if (rendition.output_stream.empty()) return job_error("every rendition needs an output stream name");
    }

    // Declared before the lock so the replaced job's puller thread is joined
    // after mutex_ is released (that join can block on a network read).
    std::unique_ptr<HlsSourcePuller> retired;
    std::lock_guard lock(mutex_);
    const std::string key = key_of(config.application, config.name);
    if (auto it = jobs_.find(key); it != jobs_.end()) {
        // Replace an existing job with the same name. Its outputs are
        // released here because the replacement rebuilds its own stores from
        // the new configuration.
        teardown_locked(it->second, retired, /*release_outputs=*/true);
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
    std::unique_ptr<HlsSourcePuller> retired; // joined after the lock is released
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(key_of(application, name));
    if (it == jobs_.end()) return job_error("no such source job");
    Job& job = it->second;
    if (job.enabled == enabled) return snapshot_locked(job);

    if (enabled) {
        job.restart_attempts = 0;
        start_locked(job); // respawn puller from stored config
    } else {
        // Disabling is an operator decision to take the stream off the air:
        // unlike a restart, its HLS links stop resolving.
        teardown_locked(job, retired, /*release_outputs=*/true);
        job.enabled = false;
        job.restart_scheduled = false;
    }
    persist_locked(job);
    return snapshot_locked(job);
}

core::Result<SourceJobSnapshot> SourceJobManager::restart(const std::string& application,
                                                          const std::string& name) {
    const std::string key = key_of(application, name);
    {
        std::lock_guard lock(mutex_);
        auto it = jobs_.find(key);
        if (it == jobs_.end()) return job_error("no such source job");
        if (!it->second.enabled) return job_error("job is disabled; enable it first");
        it->second.restart_attempts = 0;
    }
    // Same stop-outside-the-lock path the auto-restart monitor uses, so a
    // manual restart of a job whose source is hung cannot block the
    // management API's other readers while the puller's thread is joined.
    restart_job(key);

    std::lock_guard lock(mutex_);
    auto it = jobs_.find(key);
    if (it == jobs_.end()) return job_error("no such source job");
    return snapshot_locked(it->second);
}

bool SourceJobManager::remove(const std::string& application, const std::string& name) {
    std::unique_ptr<HlsSourcePuller> retired; // joined after the lock is released
    std::lock_guard lock(mutex_);
    auto it = jobs_.find(key_of(application, name));
    if (it == jobs_.end()) return false;
    teardown_locked(it->second, retired, /*release_outputs=*/true);
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
    // Detach every puller under the lock, then join them once it is
    // released: each join can wait on its own network timeout, and holding
    // the manager lock across that would block every management-API read
    // for the whole shutdown.
    std::vector<std::unique_ptr<HlsSourcePuller>> retired;
    {
        std::lock_guard lock(mutex_);
        retired.reserve(jobs_.size());
        for (auto& [key, job] : jobs_) {
            (void)key;
            std::unique_ptr<HlsSourcePuller> puller;
            teardown_locked(job, puller, /*release_outputs=*/true);
            if (puller) retired.push_back(std::move(puller));
        }
    }
    retired.clear();
}

} // namespace rtmp_server::transcoding::native
