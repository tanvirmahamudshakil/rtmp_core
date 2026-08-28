#include "rtmp_server/io/io_uring/cross_worker_router.hpp"

#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <thread>
#include <vector>

#include "rtmp_server/core/buffer.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"

namespace rtmp_server::io::io_uring {
namespace {

using protocol::commands::StreamId;

SharedMediaFrame make_frame(std::uint32_t timestamp = 0) {
    SharedMediaFrame frame;
    frame.payload = core::SharedBuffer::copy_from(std::vector<std::byte>{std::byte{1}, std::byte{2}});
    frame.timestamp = timestamp;
    return frame;
}

bool fd_is_readable(int fd) {
    std::uint64_t value = 0;
    ssize_t n = ::read(fd, &value, sizeof(value));
    return n == static_cast<ssize_t>(sizeof(value));
}

TEST(CrossWorkerRouterTest, ForwardWithoutSubscribersDoesNothing) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.forward(/*source_worker=*/0, stream, make_frame(), /*is_video=*/true, /*is_audio=*/false);
    EXPECT_TRUE(router.drain(1).empty());
    EXPECT_EQ(router.dropped_frame_count(), 0u);
}

TEST(CrossWorkerRouterTest, StickyFramesReachEveryWorkerEvenWithoutSubscribers) {
    // Regression: the publisher's AVC/AAC sequence headers and onMetadata are
    // sent once, before any viewer on another worker exists. Demand-gated
    // forwarding never delivered them there, so a cross-worker viewer got
    // media it could not decode (video with no SPS/PPS). Sticky frames must
    // fan out to every worker regardless of subscriber counts.
    CrossWorkerRouter router(3);
    StreamId stream = StreamId::next();

    router.forward(/*source_worker=*/0, stream, make_frame(), /*is_video=*/true, /*is_audio=*/false,
                   /*is_sticky=*/true);

    EXPECT_EQ(router.drain(1).size(), 1u); // no subscriber yet, still primed
    EXPECT_EQ(router.drain(2).size(), 1u);
    EXPECT_TRUE(router.drain(0).empty()); // never back to the source
    EXPECT_EQ(router.dropped_frame_count(), 0u);
}

TEST(CrossWorkerRouterTest, NonStickyMediaWithoutSubscribersIsStillDropped) {
    CrossWorkerRouter router(3);
    StreamId stream = StreamId::next();
    router.forward(/*source_worker=*/0, stream, make_frame(), /*is_video=*/true, /*is_audio=*/false,
                   /*is_sticky=*/false);
    EXPECT_TRUE(router.drain(1).empty());
    EXPECT_TRUE(router.drain(2).empty());
}

TEST(CrossWorkerRouterTest, ForwardsOnlyToWorkersWithSubscribers) {
    CrossWorkerRouter router(3);
    StreamId stream = StreamId::next();
    router.note_subscription(/*worker=*/1, stream, +1);

    router.forward(/*source_worker=*/0, stream, make_frame(), /*is_video=*/true, /*is_audio=*/false);

    auto to_worker_1 = router.drain(1);
    ASSERT_EQ(to_worker_1.size(), 1u);
    EXPECT_EQ(to_worker_1[0].kind, CrossWorkerRouter::FrameKind::Video);
    EXPECT_TRUE(router.drain(2).empty());
}

TEST(CrossWorkerRouterTest, NeverForwardsBackToSourceWorker) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.note_subscription(0, stream, +1);
    router.note_subscription(1, stream, +1);

    router.forward(/*source_worker=*/0, stream, make_frame(), /*is_video=*/false, /*is_audio=*/true);

    EXPECT_TRUE(router.drain(0).empty());
    EXPECT_EQ(router.drain(1).size(), 1u);
}

TEST(CrossWorkerRouterTest, UnsubscribeStopsForwarding) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, +1);
    router.note_subscription(1, stream, -1);

    router.forward(0, stream, make_frame(), true, false);
    EXPECT_TRUE(router.drain(1).empty());
}

TEST(CrossWorkerRouterTest, SubscriptionCountNeverUnderflowsBelowZero) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, -1); // no matching +1 yet: must clamp at 0, not wrap
    router.note_subscription(1, stream, +1);

    router.forward(0, stream, make_frame(), true, false);
    EXPECT_EQ(router.drain(1).size(), 1u); // exactly one subscriber's worth of forwarding
}

TEST(CrossWorkerRouterTest, StreamEndClearsSubscriptionBookkeeping) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, +1);
    router.on_stream_end(/*source_worker=*/0, stream);

    router.forward(0, stream, make_frame(), true, false);
    auto drained = router.drain(1);
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained.front().kind, CrossWorkerRouter::FrameKind::StreamEnd);
}

TEST(CrossWorkerRouterTest, StreamEndIsBroadcastOnceAndPrioritized) {
    CrossWorkerRouter router(3);
    StreamId active = StreamId::next();
    StreamId ended = StreamId::next();
    router.note_subscription(1, active, +1);
    router.forward(0, active, make_frame(1), true, false);

    router.on_stream_end(/*source_worker=*/0, ended);
    router.on_stream_end(/*source_worker=*/0, ended); // coalesced

    EXPECT_TRUE(router.drain(0).empty());
    auto worker_1 = router.drain(1);
    ASSERT_EQ(worker_1.size(), 2u);
    EXPECT_EQ(worker_1[0].kind, CrossWorkerRouter::FrameKind::StreamEnd);
    EXPECT_EQ(worker_1[0].stream_id, ended);
    EXPECT_EQ(worker_1[1].frame.timestamp, 1u);

    auto worker_2 = router.drain(2);
    ASSERT_EQ(worker_2.size(), 1u);
    EXPECT_EQ(worker_2[0].kind, CrossWorkerRouter::FrameKind::StreamEnd);
}

TEST(CrossWorkerRouterTest, StickyStateIsCoalescedToTheLatestFrame) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();

    router.forward(0, stream, make_frame(1), true, false, true);
    router.forward(0, stream, make_frame(2), true, false, true);

    auto drained = router.drain(1);
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained[0].frame.timestamp, 2u);
    EXPECT_TRUE(drained[0].is_sticky);
}

TEST(CrossWorkerRouterTest, QueueOverflowDropsPartialGopAndResumesAtNextKeyframe) {
    CrossWorkerRouter router(2, /*max_queue_frames_per_worker=*/2);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, +1);

    router.forward(0, stream, make_frame(1), true, false, false, true);
    router.forward(0, stream, make_frame(2), true, false);
    router.forward(0, stream, make_frame(3), true, false); // overflow: whole partial GOP is shed
    router.forward(0, stream, make_frame(4), false, true); // gated until a keyframe
    EXPECT_TRUE(router.drain(1).empty());

    router.forward(0, stream, make_frame(5), true, false, false, true);
    router.forward(0, stream, make_frame(6), false, true);
    auto drained = router.drain(1);
    EXPECT_EQ(drained.size(), 2u);
    EXPECT_EQ(drained[0].frame.timestamp, 5u);
    EXPECT_EQ(drained[1].frame.timestamp, 6u);
    EXPECT_GE(router.dropped_frame_count(), 4u);
}

TEST(CrossWorkerRouterTest, ByteLimitBoundsFewHugeFrames) {
    CrossWorkerRouter router(2, /*max_queue_frames_per_worker=*/100,
                             /*max_queue_bytes_per_worker=*/3);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, +1);

    router.forward(0, stream, make_frame(1), true, false, false, true); // 2 bytes
    router.forward(0, stream, make_frame(2), true, false); // would total 4; partial GOP shed

    EXPECT_TRUE(router.drain(1).empty());
    EXPECT_GT(router.dropped_frame_count(), 0u);
}

TEST(CrossWorkerRouterTest, BoundedDrainResignalsWhenFramesRemain) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, +1);
    router.forward(0, stream, make_frame(1), true, false);
    router.forward(0, stream, make_frame(2), true, false);

    EXPECT_EQ(router.drain(1, 1).size(), 1u);
    EXPECT_TRUE(fd_is_readable(router.wake_fd(1)));
    EXPECT_EQ(router.drain(1, 1).size(), 1u);
}

TEST(CrossWorkerRouterTest, DrainIsFifoAndClearsQueue) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, +1);

    router.forward(0, stream, make_frame(1), true, false);
    router.forward(0, stream, make_frame(2), false, true);

    auto drained = router.drain(1);
    ASSERT_EQ(drained.size(), 2u);
    EXPECT_EQ(drained[0].frame.timestamp, 1u);
    EXPECT_EQ(drained[1].frame.timestamp, 2u);

    EXPECT_TRUE(router.drain(1).empty()); // already drained
}

TEST(CrossWorkerRouterTest, WakeFdBecomesReadableAfterForward) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, +1);

    int fd = router.wake_fd(1);
    ASSERT_GE(fd, 0);

    router.forward(0, stream, make_frame(), true, false);
    EXPECT_TRUE(fd_is_readable(fd));
}

TEST(CrossWorkerRouterTest, CoalescesWakeSignalsWhileDestinationQueueIsAlreadyNonEmpty) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, +1);

    router.forward(0, stream, make_frame(1), true, false);
    router.forward(0, stream, make_frame(2), true, false);
    router.forward(0, stream, make_frame(3), true, false);

    std::uint64_t wake_count = 0;
    ASSERT_EQ(::read(router.wake_fd(1), &wake_count, sizeof(wake_count)),
              static_cast<ssize_t>(sizeof(wake_count)));
    EXPECT_EQ(wake_count, 1u);
    EXPECT_EQ(router.drain(1).size(), 3u);
}

TEST(CrossWorkerRouterTest, OutOfRangeWorkerIdsAreIgnoredNotUb) {
    CrossWorkerRouter router(2);
    StreamId stream = StreamId::next();
    router.note_subscription(5, stream, +1); // out of range: ignored
    router.forward(5, stream, make_frame(), true, false); // out of range source: ignored
    EXPECT_TRUE(router.drain(5).empty()); // out of range destination: empty, not UB
    EXPECT_EQ(router.wake_fd(5), -1);
}

TEST(CrossWorkerRouterTest, ConcurrentForwardAndSubscriptionUpdatesAreThreadSafe) {
    constexpr std::size_t kWorkers = 4;
    CrossWorkerRouter router(kWorkers);
    StreamId stream = StreamId::next();
    router.note_subscription(1, stream, +1);
    router.note_subscription(2, stream, +1);

    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < kWorkers; ++i) {
        threads.emplace_back([&router, &stream, i] {
            for (int n = 0; n < 200; ++n) {
                router.forward(static_cast<CrossWorkerRouter::WorkerId>(i), stream, make_frame(), n % 2 == 0, n % 2 != 0);
            }
        });
    }
    for (auto& t : threads) t.join();

    // No crash/UB is the primary assertion here; also sanity-check nothing
    // was forwarded back to worker 0/3 (never subscribed).
    EXPECT_TRUE(router.drain(0).empty());
    EXPECT_TRUE(router.drain(3).empty());
}

} // namespace
} // namespace rtmp_server::io::io_uring
