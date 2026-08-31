#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "rtmp_server/control/viewer_session_tracker.hpp"

namespace {

using namespace std::chrono_literals;
using rtmp_server::control::ViewerSessionTracker;

constexpr auto kWindow = 20s;

std::chrono::steady_clock::time_point base() {
    // A fixed origin far enough from the epoch that subtracting the window
    // never underflows, so tests never depend on the real clock.
    static const auto origin = std::chrono::steady_clock::now() + 1h;
    return origin;
}

TEST(ViewerSessionTracker, CountsDistinctSessions) {
    ViewerSessionTracker tracker(kWindow, 1000);
    tracker.record("aaa", base());
    tracker.record("bbb", base());
    tracker.record("aaa", base()); // same viewer refetching, not a new one
    EXPECT_EQ(tracker.active_count(base()), 2u);
}

TEST(ViewerSessionTracker, IgnoresEmptySession) {
    ViewerSessionTracker tracker(kWindow, 1000);
    tracker.record("", base());
    EXPECT_EQ(tracker.active_count(base()), 0u);
}

TEST(ViewerSessionTracker, DropsSessionsOlderThanWindow) {
    ViewerSessionTracker tracker(kWindow, 1000);
    tracker.record("stale", base());
    tracker.record("fresh", base() + 19s);
    // At base+21s "stale" is 21s old (outside the 20s window) and "fresh" is
    // 2s old.
    EXPECT_EQ(tracker.active_count(base() + 21s), 1u);
}

TEST(ViewerSessionTracker, RefetchKeepsSessionAlive) {
    ViewerSessionTracker tracker(kWindow, 1000);
    tracker.record("viewer", base());
    // A player that keeps polling must never age out, however long it watches.
    for (int i = 1; i <= 10; ++i) tracker.record("viewer", base() + std::chrono::seconds(i * 5));
    EXPECT_EQ(tracker.active_count(base() + 52s), 1u);
}

TEST(ViewerSessionTracker, CollectActiveUnionsAcrossTrackers) {
    // The ABR case: one player switching renditions appears in both ladders'
    // trackers and must be counted once.
    ViewerSessionTracker low(kWindow, 1000);
    ViewerSessionTracker high(kWindow, 1000);
    low.record("switcher", base());
    low.record("only_low", base());
    high.record("switcher", base());

    std::unordered_set<std::string> sessions;
    low.collect_active(sessions, base());
    high.collect_active(sessions, base());
    EXPECT_EQ(sessions.size(), 2u);
    EXPECT_TRUE(sessions.contains("switcher"));
    EXPECT_TRUE(sessions.contains("only_low"));
}

TEST(ViewerSessionTracker, CollectActiveExcludesExpired) {
    ViewerSessionTracker tracker(kWindow, 1000);
    tracker.record("gone", base());
    tracker.record("here", base() + 30s);
    std::unordered_set<std::string> sessions;
    tracker.collect_active(sessions, base() + 30s);
    EXPECT_EQ(sessions.size(), 1u);
    EXPECT_TRUE(sessions.contains("here"));
}

TEST(ViewerSessionTracker, BoundsMemoryUnderDistinctSessionFlood) {
    // A scrape inventing a new session ID per request must not grow without
    // bound. The cap is per-shard, so the total settles near max_sessions
    // rather than exactly at it; what matters is that it stops growing.
    constexpr std::size_t kMax = 640;
    ViewerSessionTracker tracker(kWindow, kMax);
    for (int i = 0; i < 20000; ++i) {
        // Every ID distinct and every one already outside the window when the
        // next sweep runs, so pruning is what has to reclaim them.
        tracker.record("flood-" + std::to_string(i), base() + std::chrono::seconds(i));
    }
    const auto final_count = tracker.active_count(base() + 20000s);
    EXPECT_LE(final_count, kMax);
}

TEST(ViewerSessionTracker, ConcurrentRecordsAreNotLost) {
    // The whole reason this type is sharded: many threads record at once.
    // Every distinct session recorded must be visible afterwards.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    ViewerSessionTracker tracker(kWindow, 100000);

    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&tracker, t] {
            for (int i = 0; i < kPerThread; ++i) {
                tracker.record("t" + std::to_string(t) + "-" + std::to_string(i), base());
            }
        });
    }
    for (auto& writer : writers) writer.join();

    EXPECT_EQ(tracker.active_count(base()), static_cast<std::size_t>(kThreads * kPerThread));
}

TEST(ViewerSessionTracker, ConcurrentReadersAndWritersDoNotRace) {
    // Exercises the read path against live writes; run under TSan this is the
    // regression test for the shard locking itself.
    ViewerSessionTracker tracker(kWindow, 100000);
    std::atomic<bool> stop{false};

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::unordered_set<std::string> sessions;
            tracker.collect_active(sessions, base());
            (void)tracker.active_count(base());
        }
    });
    for (int i = 0; i < 2000; ++i) tracker.record("w-" + std::to_string(i), base());
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    EXPECT_EQ(tracker.active_count(base()), 2000u);
}

} // namespace
