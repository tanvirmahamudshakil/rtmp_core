#include <gtest/gtest.h>

#include <thread>

#include "rtmp_server/media/media_handoff_queue.hpp"

namespace {

using rtmp_server::media::HandoffLimits;
using rtmp_server::media::HandoffMessage;
using rtmp_server::media::MediaHandoffQueue;
using rtmp_server::media::TimestampUnwrapper;

HandoffMessage video(bool keyframe, std::size_t bytes, std::uint32_t timestamp = 0) {
    HandoffMessage message;
    message.video = true;
    message.keyframe = keyframe;
    message.timestamp = timestamp;
    message.payload.assign(bytes, std::byte{0});
    return message;
}

HandoffMessage audio(std::size_t bytes) {
    HandoffMessage message;
    message.video = false;
    message.payload.assign(bytes, std::byte{0});
    return message;
}

HandoffMessage video_sequence_header(std::size_t bytes) {
    auto message = video(false, bytes);
    message.sequence_header = true;
    return message;
}

HandoffLimits tiny_limits(std::size_t bytes, std::size_t messages = 1024) {
    HandoffLimits limits;
    limits.max_bytes = bytes;
    limits.max_messages = messages;
    return limits;
}

TEST(MediaHandoffQueueTest, DeliversInOrderWhenTheWorkerKeepsUp) {
    MediaHandoffQueue queue;
    EXPECT_TRUE(queue.push(video(true, 10, 100)));
    EXPECT_TRUE(queue.push(video(false, 10, 133)));

    HandoffMessage out;
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.timestamp, 100u);
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.timestamp, 133u);
    EXPECT_EQ(queue.stats().queued_messages, 0u);
}

// The publisher runs on an io_uring worker shared with every other connection
// on its ring: a full queue must cost transcoded frames, never a stalled
// publish.
TEST(MediaHandoffQueueTest, DropsMidGopVideoInsteadOfBlockingThePublisher) {
    MediaHandoffQueue queue(tiny_limits(100));
    EXPECT_TRUE(queue.push(video(true, 60)));
    EXPECT_FALSE(queue.push(video(false, 60))); // would exceed 100 bytes

    const auto stats = queue.stats();
    EXPECT_EQ(stats.dropped, 1u);
    EXPECT_EQ(stats.queued_messages, 1u);
}

// Once one frame is gone the rest of that GOP references pictures the decoder
// will never see, so it is discarded too rather than decoded into garbage.
TEST(MediaHandoffQueueTest, KeepsDroppingUntilTheNextKeyframe) {
    MediaHandoffQueue queue(tiny_limits(100));
    ASSERT_TRUE(queue.push(video(true, 90)));
    ASSERT_FALSE(queue.push(video(false, 20))); // over the ceiling: drop

    HandoffMessage out;
    ASSERT_TRUE(queue.pop(out)); // drain, so there is room again

    // Room exists now, but the GOP is already broken.
    EXPECT_FALSE(queue.push(video(false, 10)));
    EXPECT_EQ(queue.stats().dropped, 2u);

    // A keyframe is a fresh decodable point and is admitted.
    EXPECT_TRUE(queue.push(video(true, 10)));
    EXPECT_TRUE(queue.push(video(false, 10)));
}

TEST(MediaHandoffQueueTest, ReportsOneResyncPerRecoveredGop) {
    MediaHandoffQueue queue(tiny_limits(100));
    ASSERT_TRUE(queue.push(video(true, 90)));
    ASSERT_FALSE(queue.push(video(false, 20)));
    EXPECT_FALSE(queue.take_resync()); // nothing recovered yet

    ASSERT_TRUE(queue.push(video(true, 10)));
    EXPECT_TRUE(queue.take_resync());
    EXPECT_FALSE(queue.take_resync()); // consumed exactly once
    EXPECT_EQ(queue.stats().resyncs, 1u);
}

// A keyframe arriving while the backlog is already over the ceiling is worth
// more than the backlog: the pending frames belong to a GOP the worker is
// behind on, and publishing them late only pushes it further behind.
TEST(MediaHandoffQueueTest, AKeyframeOverTheCeilingReplacesTheBacklog) {
    MediaHandoffQueue queue(tiny_limits(100));
    ASSERT_TRUE(queue.push(video(true, 40, 1)));
    ASSERT_TRUE(queue.push(video(false, 40, 2)));

    ASSERT_TRUE(queue.push(video(true, 40, 3))); // over the ceiling
    EXPECT_TRUE(queue.take_resync());

    HandoffMessage out;
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.timestamp, 3u); // the backlog was abandoned
    EXPECT_TRUE(out.keyframe);
    EXPECT_EQ(queue.stats().queued_messages, 0u);
}

// Losing an AudioSpecificConfig or an AVCDecoderConfigurationRecord breaks
// every frame after it, not one GOP, so these are admitted past the ceiling.
TEST(MediaHandoffQueueTest, NeverDropsAudioOrSequenceHeadersForCapacity) {
    MediaHandoffQueue queue(tiny_limits(50));
    ASSERT_TRUE(queue.push(video(true, 40)));
    EXPECT_TRUE(queue.push(audio(40)));
    EXPECT_TRUE(queue.push(video_sequence_header(40)));
    EXPECT_EQ(queue.stats().dropped, 0u);
}

// ... but a worker that has stopped consuming entirely must not be allowed to
// grow the queue without end, even on audio alone.
TEST(MediaHandoffQueueTest, StopsAudioAtTheHardCap) {
    MediaHandoffQueue queue(tiny_limits(1000));
    bool dropped = false;
    for (int i = 0; i < 100 && !dropped; ++i) {
        dropped = !queue.push(audio(200));
    }
    EXPECT_TRUE(dropped);
    EXPECT_LE(queue.stats().queued_bytes, 4u * 1000u);
}

TEST(MediaHandoffQueueTest, PopDrainsWhatIsQueuedAfterClose) {
    MediaHandoffQueue queue;
    ASSERT_TRUE(queue.push(video(true, 10, 7)));
    queue.close();

    HandoffMessage out;
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.timestamp, 7u);
    EXPECT_FALSE(queue.pop(out));
    EXPECT_FALSE(queue.push(video(true, 10)));
}

TEST(MediaHandoffQueueTest, PopBlocksUntilCloseWakesIt) {
    MediaHandoffQueue queue;
    std::thread worker([&queue] {
        HandoffMessage out;
        EXPECT_FALSE(queue.pop(out));
    });
    queue.close();
    worker.join();
}

TEST(TimestampUnwrapperTest, CarriesTheEpochAcrossA32BitWrap) {
    TimestampUnwrapper clock;
    EXPECT_EQ(clock.unwrap(0xFFFFFF00u), 0xFFFFFF00ull);
    EXPECT_EQ(clock.unwrap(0x00000100u), 0x100000100ull);
}

// A backwards step that is not a wrap (a publisher's own timestamp jitter)
// must not add an epoch: that would move the stream 49 days into the future.
TEST(TimestampUnwrapperTest, IgnoresSmallBackwardsSteps) {
    TimestampUnwrapper clock;
    EXPECT_EQ(clock.unwrap(5000), 5000ull);
    EXPECT_EQ(clock.unwrap(4900), 4900ull);
}

} // namespace
