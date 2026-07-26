#include "rtmp_server/protocol/commands/stream_ids.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <unordered_set>
#include <vector>

namespace rtmp_server::protocol::commands {
namespace {

TEST(StrongIdTest, DefaultConstructedIsInvalid) {
    StreamId id;
    EXPECT_FALSE(id.valid());
    EXPECT_FALSE(static_cast<bool>(id));
    EXPECT_EQ(id.raw(), 0u);
}

TEST(StrongIdTest, NextMintsIncreasingUniqueValidIds) {
    StreamId a = StreamId::next();
    StreamId b = StreamId::next();
    EXPECT_TRUE(a.valid());
    EXPECT_TRUE(b.valid());
    EXPECT_NE(a, b);
    EXPECT_LT(a.raw(), b.raw());
}

TEST(StrongIdTest, DistinctTagsHaveIndependentCounters) {
    // StreamId and PublisherId are unrelated types with their own counters
    // (each StrongId<Tag> specialization gets its own static atomic) — not
    // asserting exact values (order across TUs/tests isn't guaranteed) only
    // that raw() from one Tag cannot be silently used as another Tag's raw()
    // because the types are distinct at compile time.
    StreamId s = StreamId::next();
    PublisherId p = PublisherId::next();
    // Would not compile: EXPECT_EQ(s, p); — different types.
    EXPECT_TRUE(s.valid());
    EXPECT_TRUE(p.valid());
}

TEST(StrongIdTest, FromRawRoundTrips) {
    StreamId id = StreamId::next();
    StreamId reconstructed = StreamId::from_raw(id.raw());
    EXPECT_EQ(id, reconstructed);
}

TEST(StrongIdTest, EqualityComparesByValue) {
    StreamId id = StreamId::next();
    StreamId copy = id;
    EXPECT_EQ(id, copy);
}

TEST(StrongIdTest, NextIsThreadSafeAndNeverDuplicatesAcrossThreads) {
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kPerThread = 500;
    std::vector<std::thread> threads;
    std::vector<std::vector<std::uint64_t>> results(kThreads);

    for (std::size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&results, t] {
            results[t].reserve(kPerThread);
            for (std::size_t i = 0; i < kPerThread; ++i) results[t].push_back(SubscriberId::next().raw());
        });
    }
    for (auto& th : threads) th.join();

    std::unordered_set<std::uint64_t> seen;
    for (const auto& per_thread : results) {
        for (auto raw : per_thread) {
            EXPECT_TRUE(seen.insert(raw).second) << "duplicate SubscriberId raw value: " << raw;
        }
    }
    EXPECT_EQ(seen.size(), kThreads * kPerThread);
}

TEST(StreamIdRegistryTest, ResolveIsStableForTheSameAppAndKey) {
    StreamIdRegistry registry;
    StreamId first = registry.resolve("live", "alice");
    StreamId second = registry.resolve("live", "alice");
    EXPECT_EQ(first, second);
}

TEST(StreamIdRegistryTest, DifferentStreamKeysResolveToDifferentIds) {
    StreamIdRegistry registry;
    StreamId a = registry.resolve("live", "alice");
    StreamId b = registry.resolve("live", "bob");
    EXPECT_NE(a, b);
}

TEST(StreamIdRegistryTest, SameStreamKeyInDifferentAppsResolvesToDifferentIds) {
    StreamIdRegistry registry;
    StreamId a = registry.resolve("live", "alice");
    StreamId b = registry.resolve("staging", "alice");
    EXPECT_NE(a, b);
}

TEST(StreamIdRegistryTest, PublisherAndViewerResolveToTheSameStreamId) {
    // The doc's core identity requirement: "Both publisher and viewer must
    // resolve to the same internal StreamId."
    StreamIdRegistry registry;
    StreamId publisher_side = registry.resolve("live", "alice");
    StreamId viewer_side = registry.resolve("live", "alice");
    EXPECT_EQ(publisher_side, viewer_side);
}

TEST(StreamIdRegistryTest, FindReturnsNulloptForUnknownKey) {
    StreamIdRegistry registry;
    EXPECT_FALSE(registry.find("live", "nobody").has_value());
}

TEST(StreamIdRegistryTest, FindReturnsTheSameIdResolveWouldHaveReturned) {
    StreamIdRegistry registry;
    StreamId resolved = registry.resolve("live", "alice");
    auto found = registry.find("live", "alice");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, resolved);
}

TEST(StreamIdRegistryTest, ForgetThenResolveMintsAFreshId) {
    StreamIdRegistry registry;
    StreamId original = registry.resolve("live", "alice");
    registry.forget("live", "alice");
    EXPECT_FALSE(registry.find("live", "alice").has_value());
    StreamId reused_name = registry.resolve("live", "alice");
    EXPECT_NE(original, reused_name);
}

} // namespace
} // namespace rtmp_server::protocol::commands
