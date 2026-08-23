#include "rtmp_server/transcoding/native/source_job_manager.hpp"

#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <limits>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/transcoding/ffmpeg_args.hpp"

extern char** environ;

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

// The management API only ever hands source jobs H.264/AAC renditions on the
// software backend (see apps/rtmp_server/main.cpp's assignment validation);
// RenditionSpec itself carries no codec/backend fields, so that pairing is
// implicit here, same as it was in the native FFmpeg-free pipeline.
Preset preset_from_rendition(const RenditionSpec& spec) {
    Preset preset;
    preset.name = spec.name;
    preset.outgoing_stream_name = spec.output_stream;
    preset.backend = BackendKind::Software;
    preset.video_codec = VideoCodec::H264;
    preset.video_bitrate = spec.video_bitrate;
    preset.keyframe_interval = spec.gop;
    if (spec.width > 0) preset.width = spec.width;
    if (spec.height > 0) preset.height = spec.height;
    preset.fit_mode = spec.fit_mode;
    preset.audio_codec = AudioCodec::Aac;
    preset.audio_bitrate = spec.audio_bitrate;
    return preset;
}

} // namespace

SourceJobManager::SourceJobManager(Hooks hooks, SourceJobManagerOptions options,
                                   std::string hls_route_prefix)
    : hooks_(std::move(hooks)),
      options_(std::move(options)),
      backends_(options_.ffmpeg_path),
      route_prefix_(std::move(hls_route_prefix)) {
    monitor_running_.store(true);
    monitor_thread_ = std::thread([this] { monitor_loop(); });
}

SourceJobManager::~SourceJobManager() {
    stop_all();
    if (monitor_running_.exchange(false)) {
        monitor_wake_cv_.notify_all();
    }
    if (monitor_thread_.joinable()) monitor_thread_.join();
}

void SourceJobManager::monitor_loop() {
    // Polls every job's ffmpeg child roughly 5x/s: reaps exited children,
    // enforces SIGTERM->SIGKILL stop deadlines, and respawns a job whose
    // restart_at has passed (scheduled by reap_locked() when auto_restart is
    // set), mirroring TranscoderSupervisor::run()/reap_children().
    while (monitor_running_.load()) {
        {
            std::unique_lock lock(monitor_wake_mutex_);
            monitor_wake_cv_.wait_for(lock, std::chrono::milliseconds(200),
                                      [this] { return !monitor_running_.load(); });
        }
        if (!monitor_running_.load()) break;

        std::lock_guard lock(mutex_);
        reap_locked();
        enforce_stop_deadlines_locked();
        const auto now = std::chrono::steady_clock::now();
        for (auto& [key, job] : jobs_) {
            (void)key;
            if (job.enabled && !job.stop_requested && job.pid < 0 &&
                job.restart_at != std::chrono::steady_clock::time_point{} && now >= job.restart_at &&
                job.restart_attempts <= options_.max_restart_attempts) {
                spawn_locked(job);
            }
        }
    }
}

std::string SourceJobManager::master_path(const std::string& application,
                                          const std::string& name) const {
    return route_prefix_ + "/" + application + "/" + name + "/master.m3u8";
}

core::Result<std::vector<std::string>> SourceJobManager::build_argv(const SourceJobConfig& config) const {
    std::vector<std::string> args{
        options_.ffmpeg_path,
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "warning",
        "-fflags",
        "+genpts+discardcorrupt",
        "-rw_timeout",
        "15000000",
    };
    // An http(s) source is an HLS/TS pull: ffmpeg's demuxer reads whatever
    // segment bytes are already available on the wire as fast as the network
    // delivers them, with no pacing of its own. Against an upstream that
    // publishes in large chunks (e.g. 10s HLS segments), that means ffmpeg
    // decodes/encodes/pushes an entire chunk in a burst — much faster than
    // real time — then blocks until the next chunk exists, so this origin's
    // own segmenter output arrives in the same bursty pattern instead of the
    // steady per-segment cadence a live player expects, reading as stutter
    // even though average throughput matches real time. `-re` paces the read
    // to the source's own embedded timestamps, smoothing that out.
    // An rtmp:// source is already a live, real-time push from its own
    // encoder; ffmpeg's docs warn `-re` on a live input can itself introduce
    // packet loss, so it is deliberately left off for that case.
    if (config.source_url.starts_with("http://") || config.source_url.starts_with("https://")) {
        args.push_back("-re");
    }
    args.push_back("-i");
    args.push_back(config.source_url);

    const std::size_t concurrent_encoders = std::max<std::size_t>(1, config.renditions.size());
    for (const auto& spec : config.renditions) {
        const auto preset = preset_from_rendition(spec);
        const std::string destination = "rtmp://" + options_.loopback_host + ":" +
                                        std::to_string(options_.rtmp_port) + "/" + config.application +
                                        "/" + spec.output_stream;
        auto appended =
            ffmpeg_append_rendition_output(args, backends_, preset, concurrent_encoders, destination);
        if (!appended) return appended.error();
    }
    return args;
}

void SourceJobManager::publish_master_locked(const SourceJobConfig& config) {
    std::vector<hls::Rendition> master_renditions;
    master_renditions.reserve(config.renditions.size());
    const auto fps = std::max<std::uint32_t>(config.fps, 1);
    for (const auto& spec : config.renditions) {
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

void SourceJobManager::spawn_locked(Job& job) {
    std::vector<char*> argv;
    argv.reserve(job.argv.size() + 1);
    for (auto& arg : job.argv) argv.push_back(arg.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int result =
        ::posix_spawn(&pid, options_.ffmpeg_path.c_str(), nullptr, nullptr, argv.data(), environ);
    if (result != 0) {
        ++job.restart_attempts;
        job.restart_at = std::chrono::steady_clock::now() + std::chrono::seconds(job.config.restart_delay_seconds);
        job.status = "error";
        job.detail = "failed to spawn ffmpeg (errno " + std::to_string(result) + ")";
        return;
    }
    job.pid = static_cast<int>(pid);
    job.restart_at = {};
    job.terminate_sent_at.reset();
    job.stop_requested = false;
    job.started_at = std::chrono::steady_clock::now();
    job.status = "starting";
    job.detail.clear();
}

void SourceJobManager::start_locked(Job& job) {
    publish_master_locked(job.config);
    job.enabled = true;
    spawn_locked(job);
}

void SourceJobManager::teardown_locked(Job& job, bool block_until_stopped) {
    if (job.pid <= 0) {
        job.status = "stopped";
        return;
    }
    job.stop_requested = true;
    ::kill(job.pid, SIGTERM);
    job.terminate_sent_at = std::chrono::steady_clock::now();
    if (!block_until_stopped) return;

    // create()/remove()/set_enabled(false) are rare, operator-triggered
    // actions; blocking here briefly (bounded by stop_timeout, then SIGKILL)
    // keeps the same synchronous "torn down before this call returns"
    // contract the old puller->stop() had.
    const auto deadline = std::chrono::steady_clock::now() + options_.stop_timeout;
    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(job.pid, &status, WNOHANG);
        if (waited == job.pid) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(job.pid, SIGKILL);
            ::waitpid(job.pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    job.pid = -1;
    job.terminate_sent_at.reset();
    job.status = "stopped";
}

void SourceJobManager::reap_locked() {
    for (auto& [key, job] : jobs_) {
        (void)key;
        if (job.pid <= 0) continue;
        int status = 0;
        const pid_t pid = ::waitpid(job.pid, &status, WNOHANG);
        if (pid <= 0) {
            // Still running: promote "starting" to "running" once it has had
            // a moment to establish the input/outputs.
            if (job.status == "starting" &&
                std::chrono::steady_clock::now() - job.started_at > std::chrono::seconds(2)) {
                job.status = "running";
            }
            continue;
        }
        job.pid = -1;
        job.terminate_sent_at.reset();
        const bool exited_cleanly = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (job.stop_requested) {
            job.status = "stopped";
            job.stop_requested = false;
            continue;
        }
        ++job.restart_attempts;
        job.status = "error";
        job.detail = exited_cleanly ? "ffmpeg exited"
                                    : "ffmpeg exited with status " + std::to_string(status);
        if (job.enabled && job.config.auto_restart &&
            job.restart_attempts <= options_.max_restart_attempts) {
            job.restart_at =
                std::chrono::steady_clock::now() + std::chrono::seconds(job.config.restart_delay_seconds);
        }
    }
}

void SourceJobManager::enforce_stop_deadlines_locked() {
    const auto now = std::chrono::steady_clock::now();
    for (auto& [key, job] : jobs_) {
        (void)key;
        if (job.pid > 0 && job.terminate_sent_at && now - *job.terminate_sent_at >= options_.stop_timeout) {
            ::kill(job.pid, SIGKILL);
            job.terminate_sent_at = now + std::chrono::hours(24);
        }
    }
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
    snapshot.status = job.enabled ? job.status : "disabled";
    snapshot.detail = job.detail;
    snapshot.renditions = job.config.renditions;
    return snapshot;
}

core::Result<SourceJobSnapshot> SourceJobManager::create(const SourceJobConfig& config) {
    if (config.name.empty()) return job_error("source job requires an output name");
    if (config.source_url.empty()) return job_error("source job requires a source URL");
    if (auto valid = validate_source_url(config.source_url); !valid) return valid.error();
    if (config.renditions.empty()) return job_error("source job requires at least one rendition");
    for (const auto& rendition : config.renditions) {
        if (rendition.output_stream.empty()) return job_error("every rendition needs an output stream name");
    }

    // Every rendition publishes back into this server's own loopback RTMP
    // listener, which enforces the same key_validator gate as an OBS
    // publish: the target stream must already exist and be enabled, or
    // ffmpeg's push is rejected ("Stream key rejected or missing") and the
    // child exits immediately (build_argv's destination is
    // rtmp://loopback:port/application/output_stream). Create/enable each
    // output stream up front so a freshly created job doesn't spin in an
    // error/auto-restart loop until an operator notices and creates it by hand.
    if (hooks_.prepare_output) {
        for (const auto& rendition : config.renditions) {
            if (!hooks_.prepare_output(config.application, rendition.output_stream)) {
                return job_error("failed to prepare output stream '" + rendition.output_stream + "'");
            }
        }
    }

    std::lock_guard lock(mutex_);
    const std::string key = key_of(config.application, config.name);
    if (auto it = jobs_.find(key); it != jobs_.end()) {
        teardown_locked(it->second); // replace an existing job with the same name
        jobs_.erase(it);
    }

    Job job;
    job.config = config;
    auto argv = build_argv(config);
    if (!argv) return argv.error();
    job.argv = std::move(argv).value();
    start_locked(job);
    auto snapshot = snapshot_locked(job);

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
        start_locked(job); // respawn ffmpeg from stored config
    } else {
        teardown_locked(job); // stop transcoding; source's HLS output stops updating
        job.enabled = false;
    }
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
    jobs_.clear();
}

} // namespace rtmp_server::transcoding::native
