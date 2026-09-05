#include "rtmp_server/cluster/node_registry.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::cluster {
namespace {

core::Error node_error(core::ErrorCode code, std::string message) {
    return core::Error(code, core::ErrorCategory::Configuration, std::move(message));
}

// An id becomes part of a URL path and a metrics label, so it is restricted to
// what is safe in both rather than escaped at every use.
bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.';
    });
}

bool valid_field(std::string_view value, std::size_t limit) {
    if (value.size() > limit) return false;
    return std::ranges::none_of(value, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

} // namespace

std::string_view to_string(NodeRole role) noexcept {
    switch (role) {
        case NodeRole::Origin: return "origin";
        case NodeRole::Edge: return "edge";
        case NodeRole::Shield: return "shield";
        case NodeRole::Transcoder: return "transcoder";
    }
    return "edge";
}

std::optional<NodeRole> parse_node_role(std::string_view text) {
    if (text == "origin") return NodeRole::Origin;
    if (text == "edge") return NodeRole::Edge;
    if (text == "shield") return NodeRole::Shield;
    if (text == "transcoder") return NodeRole::Transcoder;
    return std::nullopt;
}

NodeRegistry::NodeRegistry(persistence::Store* store, NodeRegistryOptions options)
    : store_(store), options_(options) {}

void NodeRegistry::load_from_store() {
    if (store_ == nullptr) return;
    auto rows = store_->load_cluster_nodes();
    if (!rows) {
        RTMP_LOG(observability::LogLevel::Warn, "cluster", "node load failed",
                 {{"error", rows.error().message()}});
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& row : rows.value()) {
        if (!parse_node_role(row.role)) continue;
        nodes_[row.id] = std::move(row);
    }
}

NodeStatus NodeRegistry::status_of_locked(const persistence::ClusterNodeRow& row,
                                          std::int64_t now_unix) const {
    NodeStatus status;
    status.id = row.id;
    status.role = parse_node_role(row.role).value_or(NodeRole::Edge);
    status.address = row.address;
    status.region = row.region;
    status.last_seen_unix = row.last_seen_unix;
    status.capacity_viewers = row.capacity_viewers;
    status.active_viewers = row.active_viewers;
    status.active_publishers = row.active_publishers;
    status.draining = row.draining;
    // A heartbeat from the future (clock skew between nodes) counts as fresh
    // rather than as a node that has been silent for a negative time.
    status.seconds_since_seen = std::max<std::int64_t>(0, now_unix - row.last_seen_unix);
    status.healthy = status.seconds_since_seen <= options_.heartbeat_timeout.count();
    status.load = row.capacity_viewers == 0
                      ? 1.0
                      : static_cast<double>(row.active_viewers) /
                            static_cast<double>(row.capacity_viewers);
    return status;
}

core::Result<NodeStatus> NodeRegistry::heartbeat(const NodeHeartbeat& beat,
                                                 std::int64_t now_unix) {
    if (!valid_id(beat.id)) {
        return node_error(core::ErrorCode::InvalidConfiguration,
                          "node id must be 1-64 characters of [A-Za-z0-9._-]");
    }
    if (!valid_field(beat.address, 253) || !valid_field(beat.region, 64)) {
        return node_error(core::ErrorCode::InvalidConfiguration,
                          "node address or region is too long or contains control characters");
    }

    persistence::ClusterNodeRow row;
    row.id = beat.id;
    row.role = std::string(to_string(beat.role));
    row.address = beat.address;
    row.region = beat.region;
    row.last_seen_unix = now_unix;
    row.capacity_viewers = beat.capacity_viewers;
    row.active_viewers = beat.active_viewers;
    row.active_publishers = beat.active_publishers;
    row.draining = beat.draining;

    NodeStatus status;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        expire_locked(now_unix);
        nodes_[row.id] = row;
        status = status_of_locked(row, now_unix);
    }

    // Persisted so a control-plane restart does not blank the cluster view
    // while every node waits out its heartbeat interval. This is a write per
    // node per heartbeat -- seconds apart, not per request.
    if (store_ != nullptr) {
        if (auto saved = store_->upsert_cluster_node(row); !saved) return saved.error();
    }
    return status;
}

core::Result<void> NodeRegistry::remove(std::string_view id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nodes_.erase(std::string(id)) == 0) {
            return node_error(core::ErrorCode::NotFound, "no such cluster node");
        }
    }
    if (store_ != nullptr) {
        if (auto deleted = store_->delete_cluster_node(id); !deleted) return deleted.error();
    }
    return {};
}

void NodeRegistry::expire_locked(std::int64_t now_unix) {
    std::vector<std::string> gone;
    for (const auto& [id, row] : nodes_) {
        if (now_unix - row.last_seen_unix > options_.forget_after.count()) gone.push_back(id);
    }
    for (const auto& id : gone) {
        nodes_.erase(id);
        if (store_ != nullptr) {
            // A failure here only means the row outlives the process's view of
            // it; the next load_from_store expires it again.
            (void)store_->delete_cluster_node(id);
        }
    }
}

void NodeRegistry::expire(std::int64_t now_unix) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire_locked(now_unix);
}

std::vector<NodeStatus> NodeRegistry::list(std::int64_t now_unix) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeStatus> result;
    result.reserve(nodes_.size());
    for (const auto& [id, row] : nodes_) result.push_back(status_of_locked(row, now_unix));
    std::ranges::sort(result, [](const NodeStatus& a, const NodeStatus& b) {
        return std::tie(a.region, a.id) < std::tie(b.region, b.id);
    });
    return result;
}

std::size_t NodeRegistry::healthy_count(NodeRole role, std::int64_t now_unix) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& [id, row] : nodes_) {
        const auto status = status_of_locked(row, now_unix);
        if (status.healthy && status.role == role) ++count;
    }
    return count;
}

NodeRegistry::CapacitySnapshot NodeRegistry::capacity(std::int64_t now_unix,
                                                      double high_water) const {
    CapacitySnapshot snapshot;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, row] : nodes_) {
        const auto status = status_of_locked(row, now_unix);
        if (!status.healthy || status.role != NodeRole::Edge) continue;
        ++snapshot.healthy_edges;
        snapshot.capacity_viewers += status.capacity_viewers;
        snapshot.active_viewers += status.active_viewers;
    }
    if (snapshot.capacity_viewers > 0) {
        snapshot.utilization = static_cast<double>(snapshot.active_viewers) /
                               static_cast<double>(snapshot.capacity_viewers);
    }
    snapshot.scale_out_recommended = snapshot.healthy_edges > 0 && snapshot.utilization >= high_water;
    return snapshot;
}

std::optional<NodeStatus> NodeRegistry::locate(std::string_view region_hint,
                                               std::int64_t now_unix) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<NodeStatus> best;
    for (const auto& [id, row] : nodes_) {
        const auto candidate = status_of_locked(row, now_unix);
        if (!candidate.healthy || candidate.draining) continue;
        // A shield exists to absorb edge fan-in, and a transcoder serves no
        // viewers at all; neither is a delivery destination.
        if (candidate.role != NodeRole::Edge && candidate.role != NodeRole::Origin) continue;
        // A node reporting itself at or past its sized ceiling is full. An
        // unsized node (capacity 0, load 1.0) is not excluded by this: its load
        // simply loses every comparison.
        if (candidate.capacity_viewers != 0 &&
            candidate.active_viewers >= candidate.capacity_viewers) {
            continue;
        }
        if (!best) {
            best = candidate;
            continue;
        }

        const bool candidate_in_region = !region_hint.empty() && candidate.region == region_hint;
        const bool best_in_region = !region_hint.empty() && best->region == region_hint;
        if (candidate_in_region != best_in_region) {
            if (candidate_in_region) best = candidate;
            continue;
        }
        // An origin is the fallback, never the preference: sending viewers to
        // the box that also ingests and packages is what the edge tier exists
        // to prevent.
        const bool candidate_is_edge = candidate.role == NodeRole::Edge;
        const bool best_is_edge = best->role == NodeRole::Edge;
        if (candidate_is_edge != best_is_edge) {
            if (candidate_is_edge) best = candidate;
            continue;
        }
        if (candidate.load < best->load) best = candidate;
    }
    return best;
}

} // namespace rtmp_server::cluster
