#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/hls/playlist.hpp"
#include "rtmp_server/hls/segment_store.hpp"
#include "rtmp_server/transcoding/native/hls_source_puller.hpp"
#include "rtmp_server/transcoding/native/source_transcoder.hpp"

namespace rtmp_server::transcoding::native {

// Definition of one source-transcode job, as the management API supplies it.
struct SourceJobConfig {
    std::string application;
    std::string name;          // output base stream name
    std::string source_url;    // rtmp:// or an http(s) .m3u8 / .ts
    std::string template_name; // for display / persistence
    std::uint32_t fps = 30;
    std::vector<RenditionSpec> renditions; // each with an output_stream key
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
    std::vector<RenditionSpec> renditions;
};

// Owns the lifecycle of source-transcode jobs: for each rendition it creates a
// SegmentStore, registers it for HLS serving, wires up the master playlist, and
// runs an HlsSourcePuller. Segment-store registration and master-playlist
// declaration are injected as hooks so this stays independent of the HTTP layer.
class SourceJobManager {
public:
    struct Hooks {
        std::function<void(const std::string& application, const std::string& stream,
                           std::shared_ptr<hls::SegmentStore>)>
            register_store;
        std::function<void(const std::string& application, const std::string& stream)>
            unregister_store;
        std::function<void(const std::string& application, const std::string& master,
                           std::vector<hls::Rendition>)>
            set_renditions;
    };

    explicit SourceJobManager(Hooks hooks, std::string hls_route_prefix = "/hls");
    ~SourceJobManager();
    SourceJobManager(const SourceJobManager&) = delete;
    SourceJobManager& operator=(const SourceJobManager&) = delete;

    // Starts (or replaces) a job. The output base name is unique per application.
    [[nodiscard]] core::Result<SourceJobSnapshot> create(const SourceJobConfig& config);
    [[nodiscard]] bool remove(const std::string& application, const std::string& name);
    // Toggles a job without dropping its configuration: disabling stops the
    // puller and unregisters its segment stores (source stops transcoding,
    // its HLS output disappears); enabling restarts the pull/transcode
    // pipeline from the stored config, mirroring StreamManager::set_enabled.
    [[nodiscard]] core::Result<SourceJobSnapshot> set_enabled(const std::string& application,
                                                              const std::string& name, bool enabled);
    [[nodiscard]] std::vector<SourceJobSnapshot> list(const std::string& application) const;
    void stop_all();

private:
    struct Job {
        SourceJobConfig config;
        std::unique_ptr<HlsSourcePuller> puller;
        std::vector<std::string> output_streams; // registered stream keys
        bool enabled = true;
    };

    [[nodiscard]] std::string master_path(const std::string& application,
                                          const std::string& name) const;
    void teardown_locked(Job& job);
    void start_locked(Job& job);
    [[nodiscard]] SourceJobSnapshot snapshot_locked(const Job& job) const;

    Hooks hooks_;
    std::string route_prefix_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Job> jobs_; // key: "application/name"
};

} // namespace rtmp_server::transcoding::native
