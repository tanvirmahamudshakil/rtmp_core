#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/persistence/store.hpp"

namespace rtmp_server::cluster {

// What a node does in this deployment. The control plane runs on the origin;
// every other node reports to it.
enum class NodeRole {
    // Runs rtmp_server: accepts publishers, packages HLS/DASH.
    Origin,
    // Caddy + Varnish in front of an origin: serves viewers, holds no state.
    Edge,
    // An edge with no viewers of its own, absorbing edge fan-in for the origin.
    Shield,
    // Runs transcode work only (source jobs / ingest ladders).
    Transcoder,
};

[[nodiscard]] std::string_view to_string(NodeRole role) noexcept;
[[nodiscard]] std::optional<NodeRole> parse_node_role(std::string_view text);

// One heartbeat, as a node reports it. Everything except `id` may change
// between heartbeats: a node that is re-provisioned into another region or
// resized keeps its identity.
struct NodeHeartbeat {
    std::string id;
    NodeRole role = NodeRole::Edge;
    // How viewers reach this node: a hostname or URL base, whatever the
    // deployment's player URLs are built from.
    std::string address;
    std::string region;
    // Viewer ceiling this node was sized for (bandwidth / per-viewer bitrate).
    // Zero means "unknown", which excludes the node from load comparison but
    // not from selection.
    std::uint32_t capacity_viewers = 0;
    std::uint32_t active_viewers = 0;
    std::uint32_t active_publishers = 0;
    // Set by an operator draining a node before removing it: it keeps serving
    // the sessions it has and is never handed to a new viewer.
    bool draining = false;
};

struct NodeStatus {
    std::string id;
    NodeRole role = NodeRole::Edge;
    std::string address;
    std::string region;
    std::int64_t last_seen_unix = 0;
    std::uint32_t capacity_viewers = 0;
    std::uint32_t active_viewers = 0;
    std::uint32_t active_publishers = 0;
    bool draining = false;
    // False once the node has missed its heartbeat window. An unhealthy node is
    // never selected, but its row is kept so an operator can see what is down.
    bool healthy = true;
    std::int64_t seconds_since_seen = 0;
    // active/capacity, or 1.0 when capacity is unknown so an unsized node is
    // only chosen when nothing better exists.
    double load = 1.0;
};

struct NodeRegistryOptions {
    // A node that has not been heard from for this long is unhealthy. Nodes
    // heartbeat every few seconds, so this tolerates a few missed beats.
    std::chrono::seconds heartbeat_timeout{30};
    // ... and after this long it is forgotten entirely, so a decommissioned
    // node does not accumulate in the table forever.
    std::chrono::seconds forget_after{86400};
};

// The cluster's membership and placement table.
//
// Every node except the origin is stateless, so this is deliberately a soft
// registry: nodes announce themselves by heartbeat and disappear by going
// quiet. There is no consensus and no leader election here — the origin owns
// the table, and a deployment that loses its origin loses the control plane
// with it (see docs/clustering.md "What this is not").
//
// It answers two questions: what is running (for the operator and /metrics),
// and which node a given viewer should be sent to (for a load balancer, a
// redirect, or the player URL the panel hands out).
class NodeRegistry {
public:
    NodeRegistry(persistence::Store* store, NodeRegistryOptions options = {});

    void load_from_store();

    // Records a heartbeat, inserting the node if it is new. `now_unix` is
    // passed in rather than read from the clock so placement and expiry are
    // deterministic under test.
    [[nodiscard]] core::Result<NodeStatus> heartbeat(const NodeHeartbeat& beat,
                                                     std::int64_t now_unix);

    // Operator-initiated removal (a decommissioned node), as opposed to the
    // silent expiry of `forget_after`.
    [[nodiscard]] core::Result<void> remove(std::string_view id);

    // Control-plane-issued drain order, independent of whatever the node
    // itself reports in its own heartbeat (NodeHeartbeat::draining). Set by
    // an operator or the autoscaler on a node it cannot otherwise reach (a
    // cloud API has no SSH access), and survives every subsequent heartbeat
    // from that node until explicitly cleared or the node is removed.
    // NodeStatus::draining is `heartbeat draining || forced draining`.
    [[nodiscard]] core::Result<NodeStatus> set_forced_draining(std::string_view id, bool draining,
                                                               std::int64_t now_unix);

    [[nodiscard]] std::vector<NodeStatus> list(std::int64_t now_unix) const;

    // Picks the node a new viewer should be sent to: healthy, not draining,
    // and with capacity left. Edges are preferred over origins — an origin
    // serving viewers directly is the thing an edge tier exists to avoid — and
    // among equals the least loaded node in the requested region wins, falling
    // back to other regions rather than refusing to serve.
    [[nodiscard]] std::optional<NodeStatus> locate(std::string_view region_hint,
                                                   std::int64_t now_unix) const;

    // Drops rows that have been silent past `forget_after`. Called from the
    // same paths that read the table, so no background thread is needed.
    void expire(std::int64_t now_unix);

    [[nodiscard]] std::size_t healthy_count(NodeRole role, std::int64_t now_unix) const;

    // Least-loaded healthy, non-draining node of the given role -- used for
    // transcoder-tier job placement (see dispatch::TranscoderDispatchManager).
    // Unlike locate(), this has no region preference or edge-vs-origin
    // ordering: any healthy node of the requested role is an equally valid
    // placement target, so the choice is load alone.
    [[nodiscard]] std::optional<NodeStatus> least_loaded(NodeRole role, std::int64_t now_unix) const;

    // Aggregate edge capacity/utilisation, for an external autoscaler to poll.
    // This is a signal, not an action: nothing in this process provisions or
    // removes a node. `scale_out_recommended` is set once utilisation crosses
    // `high_water` (default 85%) with at least one healthy edge counted --
    // an empty fleet recommends nothing, since there is no utilisation to
    // measure yet, only an operator decision to add the first edge.
    struct CapacitySnapshot {
        std::size_t healthy_edges = 0;
        std::uint64_t capacity_viewers = 0;
        std::uint64_t active_viewers = 0;
        double utilization = 0.0; // active/capacity; 0 when capacity is 0
        bool scale_out_recommended = false;
    };
    [[nodiscard]] CapacitySnapshot capacity(std::int64_t now_unix,
                                           double high_water = 0.85) const;

private:
    [[nodiscard]] NodeStatus status_of_locked(const persistence::ClusterNodeRow& row,
                                              std::int64_t now_unix) const;
    void expire_locked(std::int64_t now_unix);

    persistence::Store* store_ = nullptr;
    NodeRegistryOptions options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, persistence::ClusterNodeRow> nodes_;
    // A heartbeat's own upsert never clears forced_draining (see
    // SqliteStore::upsert_cluster_node), but nodes_'s in-memory row IS
    // overwritten wholesale by every heartbeat -- so the forced flag is kept
    // here too, out of the heartbeat's reach, and re-merged into the row on
    // every heartbeat and every read.
    std::unordered_map<std::string, bool> forced_draining_;
};

} // namespace rtmp_server::cluster
