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
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/hls/playlist.hpp"
#include "rtmp_server/hls/segment_store.hpp"
#include "rtmp_server/persistence/store.hpp"
#include "rtmp_server/transcoding/native/hls_source_puller.hpp"
#include "rtmp_server/transcoding/native/source_transcoder.hpp" // RenditionSpec, FitMode

namespace rtmp_server::transcoding::native {

// Converts a PresetCatalogue-shaped "rules" JSON blob (as posted by the
// admin UI / persisted alongside a job) into the RenditionSpec list a
// SourceJobConfig needs. Shared by the management-API wiring (to validate
// and build a SourceJobConfig before calling create()) and by
// SourceJobManager::load_from_store() (to rebuild a persisted job's
// renditions on restart). Rejects anything other than H.264 + AAC on the
// software backend -- the only combination this ffmpeg-free pipeline
// supports.
[[nodiscard]] core::Result<std::vector<RenditionSpec>> parse_source_job_renditions(
    std::string_view rules);

// Definition of one source-transcode job, as the management API supplies it.
// This is the ffmpeg-free replacement for the deleted process-spawning
// SourceJobManager: instead of an ffmpeg child pushing rendition RTMP streams
// back into the server's loopback listener, each job owns one HlsSourcePuller
// that decodes/transcodes the pulled source in-process and writes segments
// directly into per-rendition SegmentStores, which this manager registers
// with HlsHttpHandler for serving.
struct SourceJobConfig {
    std::string application;
    std::string name;          // output base stream name
    std::string source_url;    // rtmp:// or http(s):// (HLS/TS), fed to HlsSourcePuller
    std::string template_name; // for display / persistence
    std::string rules;         // opaque PresetCatalogue JSON this job was created from, for reload
    std::uint32_t fps = 30;
    std::vector<RenditionSpec> renditions; // each with an output_stream key
    // If the puller reports an error (source unreachable, dropped mid-stream,
    // etc.) the manager's monitor thread restarts it automatically after this
    // many seconds, so a flaky upstream doesn't require manual re-enabling.
    bool auto_restart = true;
    std::uint32_t restart_delay_seconds = 5;
};

struct SourceJobSnapshot {
    std::string application;
    std::string name;
    std::string source_url;
    std::string template_name;
    std::string master_hls_path;
    std::string status; // "starting" | "running" | "error" | "stopped" | "disabled"
    std::string detail;
    bool enabled = true;
    bool auto_restart = true;
    std::uint32_t restart_delay_seconds = 5;
    std::vector<RenditionSpec> renditions;
};

// Tuning for SourceJobManager. Defined at namespace scope rather than nested
// inside the class because the constructor below takes it by value with a
// `= {}` default: a nested type's default member initializers are not usable
// in a default argument of its own enclosing class (clang rejects it
// outright), and `SourceJobManager::Options` still names it through the
// member alias.
struct SourceJobOptions {
    // Number of consecutive failed restarts after which the retry delay
    // stops growing. Restarts never stop entirely -- a source that is down
    // for an hour must come back on its own when the upstream returns, with
    // no operator action. See SourceJobManager::monitor_loop.
    std::uint32_t max_restart_attempts = 5;
    // Ceiling on the exponential retry delay, in seconds.
    std::uint32_t restart_backoff_cap_seconds = 60;
    // How long a restarted puller has to stay in Running before its failure
    // streak is forgiven. Without this, a job that hiccups once a day still
    // ends up at the maximum backoff after a week of otherwise healthy
    // operation.
    std::uint32_t healthy_reset_seconds = 120;
    // Segment-store sizing for a source job's own rendition outputs;
    // mirrors main.cpp's services.recorder_factory defaults for a normal
    // published stream.
    std::uint32_t live_window_segments = 6;
    std::uint32_t retention_grace_segments = 6;
    std::uint64_t max_total_bytes_per_rendition = 128u * 1024u * 1024u;
    std::uint32_t target_duration_seconds = 2;
    // Percentage (0-100) of the machine's cores reserved exclusively for
    // source-transcode work (scale+encode, including the internal threads
    // libx264/libx265 spawn for a job's own encoders). 0 (the default)
    // disables the reservation: jobs size themselves from every core on the
    // box, sharing it with RTMP ingest/HTTP/admin threads exactly as before
    // this option existed. A non-zero value confines every job's render
    // pool/puller thread to that core subset (see core::CpuPartition), so a
    // busy control-plane can never steal cycles from an in-flight encode and
    // vice versa.
    std::uint32_t transcode_cpu_reservation_percent = 0;
};

// Owns the lifecycle of source-transcode jobs: for each job it builds one
// HlsSourcePuller (decode-once, encode-per-rendition, entirely in-process —
// no external ffmpeg dependency), gives it one hls::SegmentStore per
// rendition, and registers those stores with HlsHttpHandler so the normal
// HLS-serving path picks them up with no extra code there. This class only
// has to publish the master playlist (via the set_renditions hook), manage
// each puller's lifecycle (start/stop/restart/auto-restart), and persist job
// definitions so they survive a server restart.
class SourceJobManager {
public:
    struct Hooks {
        std::function<void(const std::string& application, const std::string& master,
                           std::vector<hls::Rendition>)>
            set_renditions;
        // Registers/unregisters one rendition's SegmentStore for HLS serving.
        // Called once per rendition on create/restart and again on remove/
        // disable, mirroring HlsHttpHandler::register_stream/unregister_stream.
        std::function<void(const std::string& application, const std::string& stream,
                           std::shared_ptr<hls::SegmentStore> store)>
            register_output;
        std::function<void(const std::string& application, const std::string& stream)>
            unregister_output;
    };

    using Options = SourceJobOptions;

    // `store` is optional (nullable): when unset, jobs are neither persisted
    // nor reloaded across restarts, but otherwise function normally.
    SourceJobManager(Hooks hooks, persistence::Store* store, Options options = {},
                     std::string hls_route_prefix = "/hls");
    ~SourceJobManager();
    SourceJobManager(const SourceJobManager&) = delete;
    SourceJobManager& operator=(const SourceJobManager&) = delete;

    // Reconstructs and starts every job persisted in `store`, if one was
    // supplied. Call once at startup after construction, before serving
    // management-API requests. Errors for individual rows are logged into
    // that job's status/detail rather than aborting the whole load.
    void load_from_store();

    // Starts (or replaces) a job. The output base name is unique per application.
    [[nodiscard]] core::Result<SourceJobSnapshot> create(const SourceJobConfig& config);
    [[nodiscard]] bool remove(const std::string& application, const std::string& name);
    // Toggles a job without dropping its configuration: disabling stops the
    // puller (source stops transcoding, its HLS output stops updating);
    // enabling restarts it from the stored config.
    [[nodiscard]] core::Result<SourceJobSnapshot> set_enabled(const std::string& application,
                                                              const std::string& name, bool enabled);
    // Manual restart: tears down and reconstructs the puller from its stored
    // config, same as an auto-restart cycle but triggered on demand. No-op on
    // a disabled job -- enable it first.
    [[nodiscard]] core::Result<SourceJobSnapshot> restart(const std::string& application,
                                                          const std::string& name);
    [[nodiscard]] std::vector<SourceJobSnapshot> list(const std::string& application) const;
    void stop_all();

    // Exponential retry delay for the given consecutive-failure count,
    // capped by options.restart_backoff_cap_seconds. Pure and static so the
    // backoff schedule can be exercised without a live manager.
    [[nodiscard]] static std::chrono::seconds restart_delay_for(const SourceJobConfig& config,
                                                                const SourceJobOptions& options,
                                                                std::uint32_t attempts);

private:
    struct Job {
        SourceJobConfig config;
        std::vector<PullerRendition> renditions;
        std::unique_ptr<HlsSourcePuller> puller;
        bool enabled = true;
        std::uint32_t restart_attempts = 0;
        std::string detail_override; // set on spawn failure before the puller exists
        std::chrono::steady_clock::time_point restart_at{};
        bool restart_scheduled = false;
        // Set while a restart has detached this job's puller and is joining
        // it outside mutex_ (see restart_job). Anything that could otherwise
        // start a second puller onto the same segment stores checks this.
        bool restart_in_progress = false;
        // When the current puller first reported Running, used to forgive a
        // stale failure streak once it has been healthy for
        // healthy_reset_seconds.
        std::optional<std::chrono::steady_clock::time_point> running_since;
    };

    [[nodiscard]] std::string master_path(const std::string& application,
                                          const std::string& name) const;
    void publish_master_locked(const SourceJobConfig& config,
                               const std::vector<PullerRendition>& renditions);
    void build_renditions_locked(Job& job);
    void register_outputs_locked(const Job& job);
    void unregister_outputs_locked(const Job& job);
    void start_locked(Job& job);
    // Detaches the job's puller and returns it. Stopping a puller joins a
    // worker that may be blocked in libcurl for tens of seconds, so callers
    // let the returned owner die *after* releasing mutex_ -- holding the
    // manager lock across that join froze every management-API read
    // (list()/status) for the duration.
    [[nodiscard]] std::unique_ptr<HlsSourcePuller> detach_puller_locked(Job& job);
    void teardown_locked(Job& job, std::unique_ptr<HlsSourcePuller>& retired,
                         bool release_outputs);
    // Stop-then-start with the join performed outside mutex_. Used by both
    // the manual restart endpoint and the auto-restart monitor.
    void restart_job(const std::string& key);
    // Cores this job may use for scale+encode, given how many jobs are
    // enabled right now. Caller must hold mutex_.
    [[nodiscard]] std::uint32_t cpu_budget_locked() const;
    [[nodiscard]] SourceJobSnapshot snapshot_locked(const Job& job) const;
    void persist_locked(const Job& job);
    void monitor_loop();

    Hooks hooks_;
    persistence::Store* store_;
    Options options_;
    std::string route_prefix_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Job> jobs_; // key: "application/name"

    std::thread monitor_thread_;
    std::atomic<bool> monitor_running_{false};
    std::mutex monitor_wake_mutex_;
    std::condition_variable monitor_wake_cv_;
};

} // namespace rtmp_server::transcoding::native
