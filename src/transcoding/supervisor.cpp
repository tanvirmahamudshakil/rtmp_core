#include "rtmp_server/transcoding/supervisor.hpp"

#include <csignal>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <thread>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/transcoding/ffmpeg_args.hpp"

extern char** environ;

namespace rtmp_server::transcoding {

namespace {

using observability::LogLevel;

std::string output_key(std::string_view application, std::string_view stream) {
    return std::string(application) + "/" + std::string(stream);
}

core::Error config_error(std::string message) {
    return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Configuration,
                       std::move(message));
}

} // namespace

TranscoderSupervisor::TranscoderSupervisor(SupervisorOptions options, PresetCatalogue catalogue)
    : options_(std::move(options)),
      catalogue_(std::move(catalogue)),
      backends_(options_.ffmpeg_path) {}

TranscoderSupervisor::~TranscoderSupervisor() { stop(); }

void TranscoderSupervisor::set_prepare_output(PrepareOutput callback) {
    std::lock_guard lock(mutex_);
    prepare_output_ = std::move(callback);
}

void TranscoderSupervisor::set_renditions_ready(RenditionsReady callback) {
    std::lock_guard lock(mutex_);
    renditions_ready_ = std::move(callback);
}

core::Result<void> TranscoderSupervisor::start() {
    std::lock_guard lock(mutex_);
    if (started_) return {};
    if (!options_.enabled) return {};
    if (options_.max_active_jobs == 0 || options_.max_outputs_per_job == 0) {
        return config_error("transcoding job/output limits must be positive");
    }
    if (::access(options_.ffmpeg_path.c_str(), X_OK) != 0) {
        return config_error("transcoder executable is not runnable: " + options_.ffmpeg_path);
    }
    stopping_ = false;
    started_ = true;
    thread_ = std::thread([this] { run(); });
    return {};
}

void TranscoderSupervisor::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (!started_) return;
        commands_.push_back(Command{CommandKind::Shutdown, {}, 0, std::nullopt, {}, {}});
        cv_.notify_one();
    }
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    started_ = false;
    stopping_ = false;
}

void TranscoderSupervisor::on_publish_started(
    const protocol::commands::StreamRegistration& registration) {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_) return;
    if (managed_outputs_.contains(output_key(registration.app, registration.stream_key))) return;
    commands_.push_back(Command{CommandKind::Start, registration, registration.connection_id,
                                std::nullopt, {}, {}});
    cv_.notify_one();
}

void TranscoderSupervisor::on_publish_stopped(std::uint64_t source_connection_id) {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    commands_.push_back(Command{CommandKind::Stop, {}, source_connection_id, std::nullopt, {}, {}});
    cv_.notify_one();
}

void TranscoderSupervisor::apply_rule(Rule rule) {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_) return;
    Command command;
    command.kind = CommandKind::ApplyRule;
    command.rule = std::move(rule);
    commands_.push_back(std::move(command));
    cv_.notify_one();
}

void TranscoderSupervisor::remove_rule(std::string application, std::string source_stream) {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_) return;
    Command command;
    command.kind = CommandKind::RemoveRule;
    command.application = std::move(application);
    command.source_stream = std::move(source_stream);
    commands_.push_back(std::move(command));
    cv_.notify_one();
}

bool TranscoderSupervisor::is_managed_output(std::string_view application,
                                              std::string_view stream) const {
    std::lock_guard lock(mutex_);
    return managed_outputs_.contains(output_key(application, stream));
}

std::vector<JobSnapshot> TranscoderSupervisor::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<JobSnapshot> result;
    result.reserve(jobs_.size());
    for (const auto& [connection_id, job] : jobs_) {
        JobSnapshot item;
        item.source_connection_id = connection_id;
        item.application = job.source.app;
        item.source_stream = job.source.stream_key;
        item.process_id = job.pid;
        item.restart_attempts = job.restart_attempts;
        item.running = job.pid > 0;
        for (const auto& preset : job.presets) item.output_streams.push_back(preset.outgoing_stream_name);
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<BackendCapabilities> TranscoderSupervisor::capabilities() const {
    return backends_.probe();
}

void TranscoderSupervisor::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        while (!commands_.empty()) {
            Command command = std::move(commands_.front());
            commands_.pop_front();
            if (command.kind == CommandKind::Start) handle_start(command.registration);
            else if (command.kind == CommandKind::Stop) handle_stop(command.connection_id);
            else if (command.kind == CommandKind::ApplyRule && command.rule) {
                handle_apply_rule(std::move(*command.rule));
            } else if (command.kind == CommandKind::RemoveRule) {
                handle_remove_rule(command.application, command.source_stream);
            }
            else stopping_ = true;
        }

        reap_children();
        enforce_stop_deadlines();

        const auto now = std::chrono::steady_clock::now();
        for (auto& [id, job] : jobs_) {
            (void)id;
            if (!job.stop_requested && job.pid < 0 && job.restart_at != std::chrono::steady_clock::time_point{} &&
                now >= job.restart_at && job.restart_attempts <= options_.max_restart_attempts) {
                spawn(job);
            }
        }

        if (stopping_) {
            stop_all_children();
            if (std::ranges::none_of(jobs_, [](const auto& entry) { return entry.second.pid > 0; })) {
                jobs_.clear();
                managed_outputs_.clear();
                return;
            }
        }
        cv_.wait_for(lock, std::chrono::milliseconds(200));
    }
}

void TranscoderSupervisor::handle_start(
    const protocol::commands::StreamRegistration& registration) {
    if (jobs_.contains(registration.connection_id)) return;
    auto presets = catalogue_.match(registration.app, registration.stream_key);
    if (presets.empty()) return;
    if (jobs_.size() >= options_.max_active_jobs) {
        RTMP_LOG(LogLevel::Error, "transcoder", "job limit reached",
                 {{"application", registration.app}, {"stream", registration.stream_key}});
        return;
    }
    if (presets.size() > options_.max_outputs_per_job) {
        RTMP_LOG(LogLevel::Error, "transcoder", "preset output limit exceeded",
                 {{"application", registration.app}, {"stream", registration.stream_key}});
        return;
    }

    auto arguments = build_arguments(registration, presets);
    if (!arguments) {
        RTMP_LOG(LogLevel::Error, "transcoder", "invalid transcoding job",
                 {{"application", registration.app},
                  {"stream", registration.stream_key},
                  {"error", arguments.error().message()}});
        return;
    }

    for (const auto& preset : presets) {
        if (managed_outputs_.contains(output_key(registration.app, preset.outgoing_stream_name))) {
            RTMP_LOG(LogLevel::Error, "transcoder", "output stream already managed",
                     {{"application", registration.app}, {"stream", preset.outgoing_stream_name}});
            return;
        }
        if (prepare_output_ && !prepare_output_(registration.app, preset.outgoing_stream_name)) {
            RTMP_LOG(LogLevel::Error, "transcoder", "could not prepare output stream",
                     {{"application", registration.app}, {"stream", preset.outgoing_stream_name}});
            return;
        }
    }

    Job job;
    job.source = registration;
    job.presets = std::move(presets);
    job.argv = std::move(arguments).value();
    for (const auto& preset : job.presets) {
        managed_outputs_.insert(output_key(registration.app, preset.outgoing_stream_name));
    }
    if (renditions_ready_) renditions_ready_(registration.app, registration.stream_key, job.presets);
    auto [it, inserted] = jobs_.emplace(registration.connection_id, std::move(job));
    if (inserted) spawn(it->second);
}

void TranscoderSupervisor::handle_stop(std::uint64_t connection_id) {
    auto it = jobs_.find(connection_id);
    if (it == jobs_.end()) return;
    auto& job = it->second;
    if (job.pid < 0) {
        const auto source = job.source;
        for (const auto& preset : job.presets) {
            managed_outputs_.erase(output_key(job.source.app, preset.outgoing_stream_name));
        }
        jobs_.erase(it);
        commands_.push_back(Command{CommandKind::Start, source, source.connection_id,
                                    std::nullopt, {}, {}});
        return;
    }
    job.stop_requested = true;
    job.restart_at = {};
    if (job.pid > 0) {
        ::kill(job.pid, SIGTERM);
        job.terminate_sent_at = std::chrono::steady_clock::now();
    } else {
        for (const auto& preset : job.presets) {
            managed_outputs_.erase(output_key(job.source.app, preset.outgoing_stream_name));
        }
        jobs_.erase(it);
    }
}

void TranscoderSupervisor::handle_apply_rule(Rule rule) {
    const std::string application = rule.application;
    const std::string source_stream = rule.source_stream;
    catalogue_.upsert_rule(std::move(rule));
    auto it = std::ranges::find_if(jobs_, [&](const auto& entry) {
        return entry.second.source.app == application &&
               entry.second.source.stream_key == source_stream;
    });
    if (it == jobs_.end()) return;
    auto& job = it->second;
    job.stop_requested = true;
    job.restart_after_stop = true;
    job.restart_at = {};
    if (job.pid > 0) {
        ::kill(job.pid, SIGTERM);
        job.terminate_sent_at = std::chrono::steady_clock::now();
    }
}

void TranscoderSupervisor::handle_remove_rule(std::string_view application,
                                               std::string_view source_stream) {
    catalogue_.remove_rule(application, source_stream);
    auto it = std::ranges::find_if(jobs_, [&](const auto& entry) {
        return entry.second.source.app == application &&
               entry.second.source.stream_key == source_stream;
    });
    if (it != jobs_.end()) handle_stop(it->first);
}

void TranscoderSupervisor::spawn(Job& job) {
    std::vector<char*> argv;
    argv.reserve(job.argv.size() + 1);
    for (auto& arg : job.argv) argv.push_back(arg.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int result = ::posix_spawn(&pid, options_.ffmpeg_path.c_str(), nullptr, nullptr, argv.data(), environ);
    if (result != 0) {
        ++job.restart_attempts;
        job.restart_at = std::chrono::steady_clock::now() + options_.restart_delay;
        RTMP_LOG(LogLevel::Error, "transcoder", "worker spawn failed",
                 {{"application", job.source.app},
                  {"stream", job.source.stream_key},
                  {"error_number", std::to_string(result)}});
        return;
    }
    job.pid = static_cast<int>(pid);
    job.restart_at = {};
    job.terminate_sent_at.reset();
    RTMP_LOG(LogLevel::Info, "transcoder", "worker started",
             {{"application", job.source.app},
              {"stream", job.source.stream_key},
              {"pid", std::to_string(job.pid)},
              {"outputs", std::to_string(job.presets.size())}});
}

void TranscoderSupervisor::reap_children() {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        auto& job = it->second;
        if (job.pid <= 0) {
            ++it;
            continue;
        }
        int status = 0;
        const pid_t pid = ::waitpid(job.pid, &status, WNOHANG);
        if (pid <= 0) {
            ++it;
            continue;
        }
        job.pid = -1;
        job.terminate_sent_at.reset();
        if (job.stop_requested || stopping_) {
            const bool restart = job.restart_after_stop && !stopping_;
            const auto source = job.source;
            for (const auto& preset : job.presets) {
                managed_outputs_.erase(output_key(job.source.app, preset.outgoing_stream_name));
            }
            it = jobs_.erase(it);
            if (restart) {
                commands_.push_back(Command{CommandKind::Start, source, source.connection_id,
                                            std::nullopt, {}, {}});
            }
            continue;
        }
        ++job.restart_attempts;
        if (job.restart_attempts <= options_.max_restart_attempts) {
            job.restart_at = std::chrono::steady_clock::now() + options_.restart_delay;
            RTMP_LOG(LogLevel::Warn, "transcoder", "worker exited; restart scheduled",
                     {{"application", job.source.app},
                      {"stream", job.source.stream_key},
                      {"attempt", std::to_string(job.restart_attempts)}});
        } else {
            RTMP_LOG(LogLevel::Error, "transcoder", "worker restart budget exhausted",
                     {{"application", job.source.app}, {"stream", job.source.stream_key}});
        }
        ++it;
    }
}

void TranscoderSupervisor::enforce_stop_deadlines() {
    const auto now = std::chrono::steady_clock::now();
    for (auto& [id, job] : jobs_) {
        (void)id;
        if (job.pid > 0 && job.terminate_sent_at && now - *job.terminate_sent_at >= options_.stop_timeout) {
            ::kill(job.pid, SIGKILL);
            job.terminate_sent_at = now + std::chrono::hours(24);
        }
    }
}

void TranscoderSupervisor::stop_all_children() {
    const auto now = std::chrono::steady_clock::now();
    for (auto& [id, job] : jobs_) {
        (void)id;
        job.stop_requested = true;
        if (job.pid > 0 && !job.terminate_sent_at) {
            ::kill(job.pid, SIGTERM);
            job.terminate_sent_at = now;
        }
    }
}

core::Result<std::vector<std::string>> TranscoderSupervisor::build_arguments(
    const protocol::commands::StreamRegistration& source,
    const std::vector<Preset>& presets) const {
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
        "-i",
        "rtmp://" + options_.loopback_host + ":" + std::to_string(options_.rtmp_port) + "/" +
            source.app + "/" + source.stream_key,
    };

    const std::size_t concurrent_encoders =
        std::max<std::size_t>(1, options_.max_active_jobs * presets.size());
    for (const auto& preset : presets) {
        // The current origin's RTMP parser and MPEG-TS packager support
        // AVC/AAC. Other model values remain valid for future SRT/WebRTC
        // targets, but refusing them here is safer than emitting a stream the
        // origin cannot package or play.
        if (preset.video_codec != VideoCodec::H264 &&
            preset.video_codec != VideoCodec::Passthrough &&
            preset.video_codec != VideoCodec::Disabled) {
            return config_error("RTMP/HLS output currently supports H.264, passthrough or disabled video");
        }
        if (preset.audio_codec != AudioCodec::Aac &&
            preset.audio_codec != AudioCodec::Passthrough &&
            preset.audio_codec != AudioCodec::Disabled) {
            return config_error("RTMP/HLS output currently supports AAC, passthrough or disabled audio");
        }

        const std::string destination = "rtmp://" + options_.loopback_host + ":" +
                                        std::to_string(options_.rtmp_port) + "/" + source.app + "/" +
                                        preset.outgoing_stream_name;
        auto appended =
            ffmpeg_append_rendition_output(args, backends_, preset, concurrent_encoders, destination);
        if (!appended) return appended.error();
    }
    return args;
}

} // namespace rtmp_server::transcoding
