#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/hls/playlist.hpp"
#include "rtmp_server/transcoding/backend.hpp"
#include "rtmp_server/transcoding/native/source_transcoder.hpp" // RenditionSpec, FitMode

namespace rtmp_server::transcoding::native {

// Definition of one source-transcode job, as the management API supplies it.
struct SourceJobConfig {
    std::string application;
    std::string name;          // output base stream name
    std::string source_url;    // rtmp:// pull or http(s):// HLS/TS, fed straight to ffmpeg -i
    std::string template_name; // for display / persistence
    std::uint32_t fps = 30;
    std::vector<RenditionSpec> renditions; // each with an output_stream key
    // If the ffmpeg child dies (source unreachable, dropped mid-stream, etc.)
    // the manager's monitor loop restarts it automatically after this many
    // seconds, so a flaky upstream doesn't require manual re-enabling.
    bool auto_restart = true;
    std::uint32_t restart_delay_seconds = 5;
};

struct SourceJobSnapshot {
    std::string application;
    std::string name;
    std::string source_url;
    std::string template_name;
    std::string master_hls_path;
    std::string status;
    std::string detail;
    bool enabled = true;
    bool auto_restart = true;
    std::uint32_t restart_delay_seconds = 5;
    std::vector<RenditionSpec> renditions;
};

// Options controlling how the manager talks to ffmpeg and the server's own
// loopback RTMP listener. Mirrors transcoding::SupervisorOptions, since a
// source job's output side works exactly like TranscoderSupervisor's: each
// rendition is pushed as an ordinary `-f flv` RTMP publish back into the
// server, which segments it into HLS the same way any other published stream
// is segmented.
struct SourceJobManagerOptions {
    std::string ffmpeg_path = "/usr/bin/ffmpeg";
    std::string loopback_host = "127.0.0.1";
    std::uint16_t rtmp_port = 1935;
    std::chrono::seconds stop_timeout{5};
    std::uint32_t max_restart_attempts = 5;
};

// Owns the lifecycle of source-transcode jobs: for each job it spawns one
// ffmpeg subprocess that pulls the external source_url and pushes every
// rendition back into the server's own loopback RTMP listener as `-f flv`
// outputs — exactly how TranscoderSupervisor's renditions work, just with an
// arbitrary external URL as input instead of a locally-published stream. Each
// pushed rendition is picked up by the server's normal publish-triggered HLS
// segmenter with no extra code here; this class only has to publish the
// master playlist (via the set_renditions hook) and manage the ffmpeg child's
// lifecycle (spawn/restart/stop).
class SourceJobManager {
public:
    struct Hooks {
        std::function<void(const std::string& application, const std::string& master,
                           std::vector<hls::Rendition>)>
            set_renditions;
        // Called once per rendition before the ffmpeg child is spawned, so the
        // output stream exists and is enabled before ffmpeg tries to publish
        // to it — otherwise the RTMP listener's key_validator rejects the
        // push (same gate a normal OBS publish goes through) and the child
        // exits immediately. Mirrors TranscoderSupervisor::PrepareOutput.
        // Returning false aborts the job the same way an ffmpeg spawn failure
        // does. Optional: an unset hook preserves the previous behaviour for
        // any caller that already creates output streams itself.
        std::function<bool(std::string_view application, std::string_view output_stream)>
            prepare_output;
    };

    explicit SourceJobManager(Hooks hooks, SourceJobManagerOptions options = {},
                              std::string hls_route_prefix = "/hls");
    ~SourceJobManager();
    SourceJobManager(const SourceJobManager&) = delete;
    SourceJobManager& operator=(const SourceJobManager&) = delete;

    // Starts (or replaces) a job. The output base name is unique per application.
    [[nodiscard]] core::Result<SourceJobSnapshot> create(const SourceJobConfig& config);
    [[nodiscard]] bool remove(const std::string& application, const std::string& name);
    // Toggles a job without dropping its configuration: disabling stops the
    // ffmpeg child (source stops transcoding, its HLS output stops updating);
    // enabling restarts it from the stored config, mirroring
    // StreamManager::set_enabled.
    [[nodiscard]] core::Result<SourceJobSnapshot> set_enabled(const std::string& application,
                                                              const std::string& name, bool enabled);
    // Manual restart: tears down and respawns the ffmpeg child from its
    // stored config, same as an auto-restart cycle but triggered on demand
    // (e.g. an operator hitting "Restart" in the admin UI). No-op on a
    // disabled job — enable it first.
    [[nodiscard]] core::Result<SourceJobSnapshot> restart(const std::string& application,
                                                          const std::string& name);
    [[nodiscard]] std::vector<SourceJobSnapshot> list(const std::string& application) const;
    void stop_all();

private:
    struct Job {
        SourceJobConfig config;
        std::vector<std::string> argv;
        int pid = -1;
        std::uint32_t restart_attempts = 0;
        bool enabled = true;
        bool stop_requested = false;
        std::string status = "stopped"; // "starting" | "running" | "error" | "stopped" | "disabled"
        std::string detail;
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point restart_at{};
        std::optional<std::chrono::steady_clock::time_point> terminate_sent_at;
    };

    [[nodiscard]] std::string master_path(const std::string& application,
                                          const std::string& name) const;
    [[nodiscard]] core::Result<std::vector<std::string>> build_argv(const SourceJobConfig& config) const;
    void publish_master_locked(const SourceJobConfig& config);
    void start_locked(Job& job);
    void spawn_locked(Job& job);
    void teardown_locked(Job& job, bool block_until_stopped = true);
    [[nodiscard]] SourceJobSnapshot snapshot_locked(const Job& job) const;
    void monitor_loop();
    void reap_locked();
    void enforce_stop_deadlines_locked();

    Hooks hooks_;
    SourceJobManagerOptions options_;
    BackendRegistry backends_;
    std::string route_prefix_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Job> jobs_; // key: "application/name"

    std::thread monitor_thread_;
    std::atomic<bool> monitor_running_{false};
    std::mutex monitor_wake_mutex_;
    std::condition_variable monitor_wake_cv_;
};

} // namespace rtmp_server::transcoding::native
