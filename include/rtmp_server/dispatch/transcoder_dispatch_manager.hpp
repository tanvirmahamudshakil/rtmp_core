#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rtmp_server/cluster/node_registry.hpp"
#include "rtmp_server/core/result.hpp"
#include "rtmp_server/dispatch/agent_http_client.hpp"
#include "rtmp_server/hls/playlist.hpp"
#include "rtmp_server/persistence/store.hpp"

namespace rtmp_server::dispatch {

// One rendition of a dispatched job's ladder. Deliberately a flat, agent-
// protocol-facing struct rather than transcoding::native::RenditionSpec: this
// manager runs on the origin, which must work without the codec libraries
// only a transcoder node needs, so it cannot depend on that (codec-gated)
// header at all.
struct DispatchedRendition {
    std::string name;          // label
    std::string output_stream; // publish name this rung arrives under
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t video_bitrate = 2'500'000;
    std::uint32_t audio_bitrate = 128'000;
};

struct TranscoderJobConfig {
    std::string application; // where the ladder is published on this origin
    std::string name;        // job / base name -- master.m3u8 lives here
    std::string source_url;  // rtmp:// the transcoder node pulls from
    std::uint32_t fps = 30;
    std::vector<DispatchedRendition> renditions;
};

enum class TranscoderJobState {
    // Assigned to a node; waiting for its renditions to start publishing.
    Assigning,
    // At least one rendition is live (publishing) on this origin.
    Running,
    // No healthy transcoder node was available to assign to.
    Unassignable,
    // The assigned node stopped reporting the job as running, or every
    // rendition has gone quiet, without this manager having removed it.
    Lost,
    Stopped,
};

struct TranscoderJobStatus {
    std::string application;
    std::string name;
    std::string source_url;
    // Empty when Unassignable.
    std::string assigned_node_id;
    TranscoderJobState state = TranscoderJobState::Assigning;
    std::string detail;
    std::vector<DispatchedRendition> renditions;
};

// Decides which transcoder node runs a job and dispatches it there over HTTP;
// the actual decode/encode/push happens entirely on that node
// (apps/transcoder_agent, docs/transcoder-dispatch.md) — this manager holds
// no media pipeline itself and has no codec-library dependency, so it runs on
// every origin regardless of what is installed on it.
//
// A dispatched job's renditions arrive back at this origin as ordinary RTMP
// publishes (the transcoder node re-encodes and pushes each rung with
// RtmpPushClient, exactly as a stream target does in the other direction) —
// so HLS/DASH packaging for them is the existing, unmodified publish path.
// The only things this origin has to do beyond that: allow the publish (the
// stream names are not pre-created StreamManager rows) and declare the master
// playlist that lists the ladder.
// Hooks and options are at namespace scope, not nested, so the constructor
// below can default Options: a nested type's default member initializers are
// not usable in a default argument of its own enclosing class.
struct TranscoderDispatchHooks {
    // Declares/updates the adaptive master playlist for a job -- the same
    // hook shape SourceJobManager/IngestTranscodeManager already use.
    std::function<void(const std::string& application, const std::string& master,
                       std::vector<hls::Rendition> renditions)>
        set_renditions;
    std::function<void(const std::string& application, const std::string& master)>
        unset_renditions;
};

struct TranscoderDispatchOptions {
    // Port every transcoder_agent listens on for job assignment.
    std::uint16_t agent_port = 9200;
    AgentHttpClient::Options http;
    // Where a dispatched rendition pushes back to -- this origin's own RTMP
    // listener, reachable from the transcoder node's network (the private
    // cluster network, not necessarily the public hostname). Included in
    // every job assignment sent to an agent.
    std::string origin_rtmp_host = "127.0.0.1";
    std::uint16_t origin_rtmp_port = 1935;
};

class TranscoderDispatchManager {
public:
    using Hooks = TranscoderDispatchHooks;
    using Options = TranscoderDispatchOptions;

    TranscoderDispatchManager(cluster::NodeRegistry* node_registry, persistence::Store* store,
                             Hooks hooks, Options options = {});
    ~TranscoderDispatchManager();

    void load_from_store();

    // Picks a healthy, least-loaded transcoder node and dispatches the job to
    // it. Persists the config regardless of whether a node was available
    // right now (Unassignable), so `retry_unassigned` can place it once one
    // is; this mirrors how a source-transcode job auto-restarts rather than
    // requiring the operator to notice and recreate it.
    [[nodiscard]] core::Result<TranscoderJobStatus> create(const TranscoderJobConfig& config,
                                                          std::int64_t now_unix);
    [[nodiscard]] core::Result<void> remove(std::string_view application, std::string_view name);
    [[nodiscard]] std::vector<TranscoderJobStatus> list(std::string_view application) const;

    // True if `application`/`stream` is an expected rendition output of a
    // currently assigned job -- what the publish path's key validator
    // consults to admit a dispatched rendition with no StreamManager row.
    [[nodiscard]] bool is_expected_publish(std::string_view application,
                                          std::string_view stream) const;

    // Marks a rendition live/not-live, called from the same place the
    // recorder_factory or publish-lifecycle hooks would; updates job state
    // (Assigning -> Running on the first live rendition; Lost if every
    // rendition of a Running job goes quiet).
    void note_publish_state(std::string_view application, std::string_view stream, bool live);

    // Re-attempts placement for every job currently Unassignable or Lost.
    // Call periodically (e.g. alongside the origin's own heartbeat tick) —
    // this manager runs no background thread of its own.
    void retry_unassigned(std::int64_t now_unix);

private:
    struct Job {
        TranscoderJobConfig config;
        TranscoderJobState state = TranscoderJobState::Assigning;
        std::string detail;
        std::optional<std::string> assigned_node_id;
        std::string assigned_node_address; // for a best-effort stop-job call on removal
        std::unordered_set<std::string> live_renditions;
    };

    [[nodiscard]] static std::string key_of(std::string_view application, std::string_view name);
    [[nodiscard]] core::Result<void> dispatch_locked(Job& job, const cluster::NodeStatus& node);
    void declare_master_locked(const Job& job);
    void persist_locked(const Job& job);

    cluster::NodeRegistry* node_registry_ = nullptr;
    persistence::Store* store_ = nullptr;
    Hooks hooks_;
    Options options_;
    AgentHttpClient http_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Job> jobs_;
    // stream key ("app/stream") -> job key, for is_expected_publish/
    // note_publish_state without scanning every job.
    std::unordered_map<std::string, std::string> rendition_index_;
};

} // namespace rtmp_server::dispatch
