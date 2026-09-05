#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "rtmp_server/relay/stream_target.hpp"
#include "rtmp_server/relay/stream_target_manager.hpp"

namespace {

using rtmp_server::core::ErrorCode;
using rtmp_server::relay::redact_rtmp_url;
using rtmp_server::relay::StreamTargetConfig;
using rtmp_server::relay::StreamTargetManager;
using rtmp_server::relay::StreamTargetSink;
using rtmp_server::relay::StreamTargetState;

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

    rtmp_server::core::Result<void> upsert_stream_target(
        const rtmp_server::persistence::StreamTargetRow& row) override {
        targets[row.application + "/" + row.stream + "/" + row.name] = row;
        return {};
    }
    rtmp_server::core::Result<void> delete_stream_target(std::string_view application,
                                                          std::string_view stream,
                                                          std::string_view name) override {
        targets.erase(std::string(application) + "/" + std::string(stream) + "/" +
                      std::string(name));
        return {};
    }
    rtmp_server::core::Result<std::vector<rtmp_server::persistence::StreamTargetRow>>
    load_stream_targets() override {
        std::vector<rtmp_server::persistence::StreamTargetRow> rows;
        for (const auto& [key, row] : targets) rows.push_back(row);
        return rows;
    }

    std::unordered_map<std::string, rtmp_server::persistence::StreamTargetRow> targets;
};

StreamTargetConfig target(std::string name, std::string url, bool enabled = true) {
    StreamTargetConfig config;
    config.application = "live";
    config.stream = "main";
    config.name = std::move(name);
    config.url = std::move(url);
    config.enabled = enabled;
    return config;
}

// A target URL is a credential: its last path segment is the destination's
// stream key.
TEST(StreamTargetTest, RedactsTheStreamKeyOutOfATargetUrl) {
    EXPECT_EQ(redact_rtmp_url("rtmp://a.rtmp.youtube.com/live2/abcd-efgh-ijkl"),
              "rtmp://a.rtmp.youtube.com/live2/****ijkl");
    // A key too short to hide behind a suffix is masked entirely.
    EXPECT_EQ(redact_rtmp_url("rtmp://origin-2.internal/live/key"),
              "rtmp://origin-2.internal/live/****");
    EXPECT_EQ(redact_rtmp_url("not-a-url"), "not-a-url");
}

TEST(StreamTargetTest, BacksOffPerConsecutiveFailureAndThenHoldsAtTheCap) {
    auto config = target("cdn", "rtmp://example.invalid/live/key");
    config.restart_delay_seconds = 5;
    EXPECT_EQ(StreamTargetSink::retry_delay_for(config, 0), std::chrono::seconds(5));
    EXPECT_EQ(StreamTargetSink::retry_delay_for(config, 1), std::chrono::seconds(10));
    EXPECT_EQ(StreamTargetSink::retry_delay_for(config, 2), std::chrono::seconds(20));
    EXPECT_EQ(StreamTargetSink::retry_delay_for(config, 3), std::chrono::seconds(40));
    // Capped, and it stays capped rather than giving up: an ingest that is down
    // for an hour must come back on its own.
    EXPECT_EQ(StreamTargetSink::retry_delay_for(config, 4), std::chrono::seconds(60));
    EXPECT_EQ(StreamTargetSink::retry_delay_for(config, 50), std::chrono::seconds(60));
}

TEST(StreamTargetManagerTest, StoresATargetAndReportsItAsWaiting) {
    FakeStore store;
    StreamTargetManager manager(&store);

    auto status = manager.upsert(target("cdn", "rtmp://ingest.example.com/live/abcdefgh"));
    ASSERT_TRUE(status) << status.error().message();
    EXPECT_EQ(status.value().state, StreamTargetState::Stopped);
    EXPECT_EQ(status.value().detail, "waiting for a publisher");
    // The API must never hand the key back out.
    EXPECT_EQ(status.value().url_redacted, "rtmp://ingest.example.com/live/****efgh");
    EXPECT_EQ(store.targets.size(), 1u);
}

TEST(StreamTargetManagerTest, RejectsATargetThatIsNotAPublishableRtmpUrl) {
    FakeStore store;
    StreamTargetManager manager(&store);
    EXPECT_FALSE(manager.upsert(target("cdn", "https://ingest.example.com/live/key")));
    EXPECT_FALSE(manager.upsert(target("cdn", "rtmp://ingest.example.com/live")));
    EXPECT_FALSE(manager.upsert(target("", "rtmp://ingest.example.com/live/key")));
    EXPECT_TRUE(store.targets.empty());
}

// Every target is a full outbound copy of the publish, so the fan-out is
// bounded rather than left to whoever calls the API.
TEST(StreamTargetManagerTest, BoundsHowManyTargetsOneStreamMayFanOutTo) {
    FakeStore store;
    StreamTargetManager manager(&store);
    for (std::size_t i = 0; i < StreamTargetManager::kMaxTargetsPerStream; ++i) {
        ASSERT_TRUE(manager.upsert(
            target("cdn-" + std::to_string(i), "rtmp://ingest.example.com/live/key" + std::to_string(i))));
    }
    auto extra = manager.upsert(target("one-too-many", "rtmp://ingest.example.com/live/key"));
    ASSERT_FALSE(extra);
    EXPECT_EQ(extra.error().code(), ErrorCode::ResourceExhausted);
    // Replacing an existing target is not adding one.
    EXPECT_TRUE(manager.upsert(target("cdn-0", "rtmp://ingest.example.com/live/rotated")));
}

TEST(StreamTargetManagerTest, RemovesATarget) {
    FakeStore store;
    StreamTargetManager manager(&store);
    ASSERT_TRUE(manager.upsert(target("cdn", "rtmp://ingest.example.com/live/key")));

    ASSERT_TRUE(manager.remove("live", "main", "cdn"));
    EXPECT_TRUE(manager.list("live").empty());
    EXPECT_TRUE(store.targets.empty());

    auto missing = manager.remove("live", "main", "cdn");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST(StreamTargetManagerTest, DisablingATargetKeepsItConfigured) {
    FakeStore store;
    StreamTargetManager manager(&store);
    ASSERT_TRUE(manager.upsert(target("cdn", "rtmp://ingest.example.com/live/key")));

    auto disabled = manager.set_enabled("live", "main", "cdn", false);
    ASSERT_TRUE(disabled);
    EXPECT_FALSE(disabled.value().enabled);
    EXPECT_EQ(disabled.value().detail, "disabled");
    ASSERT_EQ(manager.list("live").size(), 1u);
    EXPECT_FALSE(store.targets.begin()->second.enabled);

    // A publish must not start a push for a disabled target.
    EXPECT_EQ(manager.create_sink("live", "main"), nullptr);
}

TEST(StreamTargetManagerTest, CreatesNoSinkForAStreamWithNoTargets) {
    FakeStore store;
    StreamTargetManager manager(&store);
    ASSERT_TRUE(manager.upsert(target("cdn", "rtmp://ingest.example.com/live/key")));
    EXPECT_EQ(manager.create_sink("live", "other"), nullptr);
    EXPECT_EQ(manager.create_sink("other", "main"), nullptr);
}

TEST(StreamTargetManagerTest, RebuildsPersistedTargetsOnRestart) {
    FakeStore store;
    {
        StreamTargetManager manager(&store);
        auto relay = target("origin-2", "rtmp://origin-2.internal/live/relaykey");
        relay.relay = true;
        ASSERT_TRUE(manager.upsert(relay));
    }
    StreamTargetManager restarted(&store);
    restarted.load_from_store();
    const auto targets = restarted.list("live");
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets.front().name, "origin-2");
    EXPECT_TRUE(targets.front().relay);
}

// One bad stored row must not take every other target down with it.
TEST(StreamTargetManagerTest, SkipsAStoredRowWhoseUrlNoLongerValidates) {
    FakeStore store;
    rtmp_server::persistence::StreamTargetRow broken;
    broken.application = "live";
    broken.stream = "main";
    broken.name = "broken";
    broken.url = "not-a-url";
    store.targets["live/main/broken"] = broken;

    rtmp_server::persistence::StreamTargetRow good;
    good.application = "live";
    good.stream = "main";
    good.name = "cdn";
    good.url = "rtmp://ingest.example.com/live/key";
    store.targets["live/main/cdn"] = good;

    StreamTargetManager manager(&store);
    manager.load_from_store();
    const auto targets = manager.list("live");
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets.front().name, "cdn");
}

TEST(StreamTargetManagerTest, ListsOnlyTheRequestedApplication) {
    FakeStore store;
    StreamTargetManager manager(&store);
    ASSERT_TRUE(manager.upsert(target("cdn", "rtmp://ingest.example.com/live/key")));

    auto other = target("cdn", "rtmp://ingest.example.com/live/key2");
    other.application = "sports";
    other.stream = "match";
    ASSERT_TRUE(manager.upsert(other));

    EXPECT_EQ(manager.list("live").size(), 1u);
    EXPECT_EQ(manager.list("sports").size(), 1u);
    EXPECT_EQ(manager.list("").size(), 2u);
    EXPECT_EQ(manager.active_target_count(), 0u);
}

} // namespace
