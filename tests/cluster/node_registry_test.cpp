#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "rtmp_server/cluster/node_registry.hpp"

namespace {

using rtmp_server::cluster::NodeHeartbeat;
using rtmp_server::cluster::NodeRegistry;
using rtmp_server::cluster::NodeRegistryOptions;
using rtmp_server::cluster::NodeRole;
using rtmp_server::core::ErrorCode;

class FakeStore final : public rtmp_server::persistence::Store {
public:
    rtmp_server::core::Result<void> upsert_application(
        const rtmp_server::persistence::ApplicationRow&) override {
        return {};
    }
    rtmp_server::core::Result<void> delete_application(std::string_view) override { return {}; }
    rtmp_server::core::Result<std::vector<rtmp_server::persistence::ApplicationRow>>
    load_applications() override {
        return std::vector<rtmp_server::persistence::ApplicationRow>{};
    }
    rtmp_server::core::Result<void> upsert_stream(
        const rtmp_server::persistence::StreamRow&) override {
        return {};
    }
    rtmp_server::core::Result<void> delete_stream(std::string_view, std::string_view) override {
        return {};
    }
    rtmp_server::core::Result<std::vector<rtmp_server::persistence::StreamRow>> load_streams()
        override {
        return std::vector<rtmp_server::persistence::StreamRow>{};
    }

    rtmp_server::core::Result<void> upsert_cluster_node(
        const rtmp_server::persistence::ClusterNodeRow& row) override {
        nodes[row.id] = row;
        return {};
    }
    rtmp_server::core::Result<void> delete_cluster_node(std::string_view id) override {
        nodes.erase(std::string(id));
        return {};
    }
    rtmp_server::core::Result<std::vector<rtmp_server::persistence::ClusterNodeRow>>
    load_cluster_nodes() override {
        std::vector<rtmp_server::persistence::ClusterNodeRow> rows;
        for (const auto& [id, row] : nodes) rows.push_back(row);
        return rows;
    }

    std::unordered_map<std::string, rtmp_server::persistence::ClusterNodeRow> nodes;
};

NodeHeartbeat beat(std::string id, NodeRole role, std::string region,
                   std::uint32_t capacity = 1000, std::uint32_t active = 0) {
    NodeHeartbeat heartbeat;
    heartbeat.id = std::move(id);
    heartbeat.role = role;
    heartbeat.address = heartbeat.id + ".example.com";
    heartbeat.region = std::move(region);
    heartbeat.capacity_viewers = capacity;
    heartbeat.active_viewers = active;
    return heartbeat;
}

TEST(NodeRegistryTest, RecordsAndReportsAHeartbeat) {
    FakeStore store;
    NodeRegistry registry(&store, {});

    auto status = registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu", 1000, 250), 1'000);
    ASSERT_TRUE(status) << status.error().message();
    EXPECT_TRUE(status.value().healthy);
    EXPECT_EQ(status.value().address, "edge-1.example.com");
    EXPECT_DOUBLE_EQ(status.value().load, 0.25);
    EXPECT_EQ(store.nodes.size(), 1u);
    EXPECT_EQ(registry.list(1'000).size(), 1u);
}

TEST(NodeRegistryTest, RejectsAnIdThatWouldNotBeSafeInAUrlOrMetricLabel) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    auto slash = registry.heartbeat(beat("edge/1", NodeRole::Edge, "eu"), 1'000);
    ASSERT_FALSE(slash);
    EXPECT_EQ(slash.error().code(), ErrorCode::InvalidConfiguration);
    EXPECT_FALSE(registry.heartbeat(beat("", NodeRole::Edge, "eu"), 1'000));
    EXPECT_TRUE(store.nodes.empty());
}

// A node goes unhealthy by going quiet; nothing has to tell the registry that
// it died, because nothing would be able to.
TEST(NodeRegistryTest, MarksANodeUnhealthyOnceItStopsHeartbeating) {
    FakeStore store;
    NodeRegistryOptions options;
    options.heartbeat_timeout = std::chrono::seconds(30);
    NodeRegistry registry(&store, options);
    ASSERT_TRUE(registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu"), 1'000));

    EXPECT_TRUE(registry.list(1'030).front().healthy);
    EXPECT_FALSE(registry.list(1'031).front().healthy);
    // The row stays: an operator has to be able to see what is down.
    EXPECT_EQ(registry.list(1'031).size(), 1u);
    EXPECT_EQ(registry.healthy_count(NodeRole::Edge, 1'031), 0u);
}

TEST(NodeRegistryTest, ForgetsANodeThatHasBeenSilentForTooLong) {
    FakeStore store;
    NodeRegistryOptions options;
    options.forget_after = std::chrono::seconds(3600);
    NodeRegistry registry(&store, options);
    ASSERT_TRUE(registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu"), 1'000));

    registry.expire(4'600);
    EXPECT_EQ(registry.list(4'600).size(), 1u);
    registry.expire(4'601);
    EXPECT_TRUE(registry.list(4'601).empty());
    EXPECT_TRUE(store.nodes.empty());
}

TEST(NodeRegistryTest, RebuildsTheClusterViewFromTheStore) {
    FakeStore store;
    {
        NodeRegistry registry(&store, {});
        ASSERT_TRUE(registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu"), 1'000));
    }
    NodeRegistry restarted(&store, {});
    restarted.load_from_store();
    const auto nodes = restarted.list(1'000);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes.front().id, "edge-1");
    EXPECT_EQ(nodes.front().role, NodeRole::Edge);
}

TEST(NodeRegistryTest, RemovesADecommissionedNode) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    ASSERT_TRUE(registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu"), 1'000));

    ASSERT_TRUE(registry.remove("edge-1"));
    EXPECT_TRUE(registry.list(1'000).empty());
    EXPECT_TRUE(store.nodes.empty());
    auto missing = registry.remove("edge-1");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST(NodeRegistryTest, LocatePrefersTheLeastLoadedNode) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    ASSERT_TRUE(registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu", 1000, 900), 1'000));
    ASSERT_TRUE(registry.heartbeat(beat("edge-2", NodeRole::Edge, "eu", 1000, 100), 1'000));

    const auto chosen = registry.locate("", 1'000);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->id, "edge-2");
}

// Region is a stronger signal than load: a viewer on another continent gains
// more from a nearby edge than from a marginally emptier one.
TEST(NodeRegistryTest, LocatePrefersTheRequestedRegionOverLoad) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    ASSERT_TRUE(registry.heartbeat(beat("edge-eu", NodeRole::Edge, "eu", 1000, 800), 1'000));
    ASSERT_TRUE(registry.heartbeat(beat("edge-us", NodeRole::Edge, "us", 1000, 10), 1'000));

    ASSERT_EQ(registry.locate("eu", 1'000)->id, "edge-eu");
    ASSERT_EQ(registry.locate("us", 1'000)->id, "edge-us");
    // An unknown region still gets served, just not preferentially.
    EXPECT_TRUE(registry.locate("ap", 1'000).has_value());
}

TEST(NodeRegistryTest, LocateFallsBackToAnOriginOnlyWhenNoEdgeCanServe) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    ASSERT_TRUE(registry.heartbeat(beat("origin-1", NodeRole::Origin, "eu", 1000, 0), 1'000));
    EXPECT_EQ(registry.locate("eu", 1'000)->id, "origin-1");

    // A healthy edge takes precedence even when the origin is emptier.
    ASSERT_TRUE(registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu", 1000, 500), 1'000));
    EXPECT_EQ(registry.locate("eu", 1'000)->id, "edge-1");
}

TEST(NodeRegistryTest, LocateSkipsUnhealthyDrainingAndFullNodes) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    ASSERT_TRUE(registry.heartbeat(beat("edge-stale", NodeRole::Edge, "eu", 1000, 0), 1'000));

    auto draining = beat("edge-draining", NodeRole::Edge, "eu", 1000, 0);
    draining.draining = true;
    ASSERT_TRUE(registry.heartbeat(draining, 2'000));
    ASSERT_TRUE(registry.heartbeat(beat("edge-full", NodeRole::Edge, "eu", 1000, 1000), 2'000));

    // edge-stale last reported at t=1000, so at t=2000 it is well past the
    // 30 s heartbeat window; the other two are excluded on their own terms.
    EXPECT_FALSE(registry.locate("eu", 2'000).has_value());

    ASSERT_TRUE(registry.heartbeat(beat("edge-ok", NodeRole::Edge, "eu", 1000, 999), 2'000));
    ASSERT_EQ(registry.locate("eu", 2'000)->id, "edge-ok");
}

// Shields absorb edge fan-in and transcoders serve no viewers; neither is a
// delivery destination.
TEST(NodeRegistryTest, LocateNeverReturnsAShieldOrTranscoder) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    ASSERT_TRUE(registry.heartbeat(beat("shield-1", NodeRole::Shield, "eu"), 1'000));
    ASSERT_TRUE(registry.heartbeat(beat("transcoder-1", NodeRole::Transcoder, "eu"), 1'000));
    EXPECT_FALSE(registry.locate("eu", 1'000).has_value());
}

TEST(NodeRegistryTest, ClockSkewFromANodeDoesNotMakeItLookSilent) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    // Heartbeat stamped in the control plane's future.
    ASSERT_TRUE(registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu"), 2'000));
    const auto status = registry.list(1'000).front();
    EXPECT_EQ(status.seconds_since_seen, 0);
    EXPECT_TRUE(status.healthy);
}

TEST(NodeRegistryTest, CapacityAggregatesHealthyEdgesOnly) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    ASSERT_TRUE(registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu", 1000, 700), 1'000));
    ASSERT_TRUE(registry.heartbeat(beat("edge-2", NodeRole::Edge, "eu", 1000, 200), 1'000));
    // Neither an origin nor a stale node should count toward edge capacity.
    ASSERT_TRUE(registry.heartbeat(beat("origin-1", NodeRole::Origin, "eu", 5000, 4900), 1'000));
    ASSERT_TRUE(registry.heartbeat(beat("edge-stale", NodeRole::Edge, "eu", 1000, 0), 500));

    const auto snapshot = registry.capacity(1'000);
    EXPECT_EQ(snapshot.healthy_edges, 2u);
    EXPECT_EQ(snapshot.capacity_viewers, 2000u);
    EXPECT_EQ(snapshot.active_viewers, 900u);
    EXPECT_DOUBLE_EQ(snapshot.utilization, 0.45);
    EXPECT_FALSE(snapshot.scale_out_recommended);
}

TEST(NodeRegistryTest, RecommendsScalingOutPastTheHighWaterMark) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    ASSERT_TRUE(registry.heartbeat(beat("edge-1", NodeRole::Edge, "eu", 1000, 900), 1'000));

    EXPECT_TRUE(registry.capacity(1'000).scale_out_recommended);
    EXPECT_FALSE(registry.capacity(1'000, /*high_water=*/0.95).scale_out_recommended);
}

// An empty fleet has no utilisation to measure, so it must not falsely
// recommend scaling out -- that decision (adding the first edge) is an
// operator's, not a signal this table can produce.
TEST(NodeRegistryTest, RecommendsNothingWithNoHealthyEdges) {
    FakeStore store;
    NodeRegistry registry(&store, {});
    EXPECT_FALSE(registry.capacity(1'000).scale_out_recommended);
    EXPECT_EQ(registry.capacity(1'000).healthy_edges, 0u);
}

TEST(NodeRoleTest, RoundTripsEveryRoleName) {
    for (const auto role : {NodeRole::Origin, NodeRole::Edge, NodeRole::Shield,
                            NodeRole::Transcoder}) {
        EXPECT_EQ(rtmp_server::cluster::parse_node_role(rtmp_server::cluster::to_string(role)), role);
    }
    EXPECT_FALSE(rtmp_server::cluster::parse_node_role("router").has_value());
}

} // namespace
