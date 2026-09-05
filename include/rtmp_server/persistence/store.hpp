#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::persistence {

// Plain data rows persisted for the management domain (Phase 8's
// management::Application/Stream), deliberately not the same types as
// management::Application/Stream: persistence should not force the domain
// layer to shape its structs around a storage engine, and vice versa —
// StreamManager maps between the two at its persistence call sites.
struct ApplicationRow {
    std::string name;
    bool enabled = true;
};

struct StreamRow {
    std::string application;
    std::string name;
    std::string key_hash; // sha256_hex of the raw key — see core/hmac.hpp
    bool enabled = true;
    bool recording_enabled = false;
    std::int64_t created_at_unix = 0;
};

struct TranscodingAssignmentRow {
    std::string application;
    std::string source_stream;
    std::string template_name;
    std::string rules;
};

// A source-transcode job: pull an external URL and re-encode it per
// `template_name`'s presets. `rules` is the same opaque PresetCatalogue JSON
// shape as TranscodingAssignmentRow::rules, persisted so the job's rendition
// ladder can be rebuilt on restart without contacting the admin UI again.
struct SourceJobRow {
    std::string application;
    std::string name;
    std::string source_url;
    std::string template_name;
    std::string rules;
    bool auto_restart = true;
    std::uint32_t restart_delay_seconds = 5;
    bool enabled = true;
};

// One outbound push destination for a locally published stream: another
// origin of this deployment (`relay`), or an external ingest such as a CDN or
// social platform. `url` carries the destination's stream key, so it is stored
// like a credential -- never returned by the API in full.
struct StreamTargetRow {
    std::string application;
    std::string stream;
    std::string name;
    std::string url;
    bool enabled = true;
    bool relay = false;
};

// A designated fallback ingest for a local stream: when the primary
// publisher is absent for `failover_after_seconds`, this URL is played and
// its media takes over packaging until the primary returns.
struct BackupPublisherRow {
    std::string application;
    std::string stream;
    std::string backup_url;
    bool enabled = true;
    std::uint32_t failover_after_seconds = 15;
};

// A job dispatched to a transcoder-tier node: which node (if any) currently
// runs it, and enough of the job's own definition to redispatch it after a
// control-plane restart. `renditions_json` is an opaque array this layer does
// not interpret, same treatment as every other opaque-JSON-blob field this
// store round-trips untouched (TranscodingAssignmentRow::rules, etc.).
struct TranscoderJobRow {
    std::string application;
    std::string name;
    std::string source_url;
    std::uint32_t fps = 30;
    std::string renditions_json;
};

// A node of this deployment as last seen by the control plane: an origin, an
// HTTP cache edge, an origin shield, or a transcoder. Heartbeats refresh
// `last_seen_unix`; a node that stops heartbeating is reported unhealthy and
// stops being handed out for playback, but its row survives a restart of
// either side.
struct ClusterNodeRow {
    std::string id;
    std::string role;
    std::string address;
    std::string region;
    std::int64_t last_seen_unix = 0;
    std::uint32_t capacity_viewers = 0;
    std::uint32_t active_viewers = 0;
    std::uint32_t active_publishers = 0;
    bool draining = false;
    // Set by the control plane (an operator or an autoscaler), independent of
    // whatever the node itself reports in `draining` above. Survives the
    // node's own heartbeats -- a node that cannot be reached directly (an
    // autoscaler acting through a cloud API has no SSH access to it) can
    // still be told to stop taking new viewers before it is torn down.
    bool forced_draining = false;
};

// A transcoding template as authored in the admin panel: a name plus its list
// of encoding presets. The preset list is opaque JSON to the store, same as
// TranscodingAssignmentRow::rules — the control layer encodes/decodes it.
struct TemplateRow {
    std::string id;
    std::string name;
    std::string presets_json;
};

// Storage-engine-independent persistence contract for the management
// domain (Phase 9, docs/rtmp_promot.md "Persistence"). Deliberately narrow —
// just enough for StreamManager to survive a restart — not a general ORM.
// Implementations must never block the RTMP media path: every call here is
// expected to be invoked only from management-API-rate code (create/rotate/
// enable/disable/delete), never per-packet (see docs/rtmp_promot.md
// "Persistence" — "Do not perform blocking database queries for every media
// packet").
class Store {
public:
    virtual ~Store() = default;

    virtual core::Result<void> upsert_application(const ApplicationRow& row) = 0;
    virtual core::Result<void> delete_application(std::string_view name) = 0;
    [[nodiscard]] virtual core::Result<std::vector<ApplicationRow>> load_applications() = 0;

    virtual core::Result<void> upsert_stream(const StreamRow& row) = 0;
    virtual core::Result<void> delete_stream(std::string_view application, std::string_view name) = 0;
    [[nodiscard]] virtual core::Result<std::vector<StreamRow>> load_streams() = 0;

    // Optional for storage implementations used by older unit tests. The
    // production SQLite store overrides all three methods.
    virtual core::Result<void> upsert_transcoding_assignment(const TranscodingAssignmentRow&) {
        return core::Result<void>{};
    }
    virtual core::Result<void> delete_transcoding_assignment(std::string_view, std::string_view) {
        return core::Result<void>{};
    }
    [[nodiscard]] virtual core::Result<std::vector<TranscodingAssignmentRow>>
    load_transcoding_assignments() {
        return std::vector<TranscodingAssignmentRow>{};
    }

    // Optional for storage implementations used by older unit tests. The
    // production SQLite store overrides all three methods.
    virtual core::Result<void> upsert_source_job(const SourceJobRow&) { return core::Result<void>{}; }
    virtual core::Result<void> delete_source_job(std::string_view, std::string_view) {
        return core::Result<void>{};
    }
    [[nodiscard]] virtual core::Result<std::vector<SourceJobRow>> load_source_jobs() {
        return std::vector<SourceJobRow>{};
    }

    // Optional for storage implementations used by older unit tests. The
    // production SQLite store overrides all three methods.
    virtual core::Result<void> upsert_stream_target(const StreamTargetRow&) {
        return core::Result<void>{};
    }
    virtual core::Result<void> delete_stream_target(std::string_view, std::string_view,
                                                    std::string_view) {
        return core::Result<void>{};
    }
    [[nodiscard]] virtual core::Result<std::vector<StreamTargetRow>> load_stream_targets() {
        return std::vector<StreamTargetRow>{};
    }

    // Optional for storage implementations used by older unit tests. The
    // production SQLite store overrides all three methods.
    virtual core::Result<void> upsert_backup_publisher(const BackupPublisherRow&) {
        return core::Result<void>{};
    }
    virtual core::Result<void> delete_backup_publisher(std::string_view, std::string_view) {
        return core::Result<void>{};
    }
    [[nodiscard]] virtual core::Result<std::vector<BackupPublisherRow>> load_backup_publishers() {
        return std::vector<BackupPublisherRow>{};
    }

    // Optional for storage implementations used by older unit tests. The
    // production SQLite store overrides all three methods.
    virtual core::Result<void> upsert_transcoder_job(const TranscoderJobRow&) {
        return core::Result<void>{};
    }
    virtual core::Result<void> delete_transcoder_job(std::string_view, std::string_view) {
        return core::Result<void>{};
    }
    [[nodiscard]] virtual core::Result<std::vector<TranscoderJobRow>> load_transcoder_jobs() {
        return std::vector<TranscoderJobRow>{};
    }

    // Optional for storage implementations used by older unit tests. The
    // production SQLite store overrides all three methods.
    virtual core::Result<void> upsert_cluster_node(const ClusterNodeRow&) {
        return core::Result<void>{};
    }
    virtual core::Result<void> delete_cluster_node(std::string_view) { return core::Result<void>{}; }
    // Optional; the production SQLite store overrides it. Never resets the
    // rest of the row -- see upsert_cluster_node's own comment on why the two
    // are kept as separate write paths.
    virtual core::Result<void> update_cluster_node_forced_draining(std::string_view, bool) {
        return core::Result<void>{};
    }
    [[nodiscard]] virtual core::Result<std::vector<ClusterNodeRow>> load_cluster_nodes() {
        return std::vector<ClusterNodeRow>{};
    }

    // Optional for storage implementations used by older unit tests. The
    // production SQLite store overrides all three methods.
    virtual core::Result<void> upsert_template(const TemplateRow&) { return core::Result<void>{}; }
    virtual core::Result<void> delete_template(std::string_view) { return core::Result<void>{}; }
    [[nodiscard]] virtual core::Result<std::vector<TemplateRow>> load_templates() {
        return std::vector<TemplateRow>{};
    }
};

} // namespace rtmp_server::persistence
