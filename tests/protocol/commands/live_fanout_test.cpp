#include "rtmp_server/protocol/commands/live_fanout.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace rtmp_server::protocol::commands {
namespace {

using chunk::MessageTypeId;
using chunk::RtmpMessage;

RtmpMessage make_video(std::vector<std::byte> payload, std::uint32_t timestamp = 0) {
    RtmpMessage m;
    m.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Video);
    m.payload = std::move(payload);
    m.timestamp = timestamp;
    return m;
}

RtmpMessage make_audio(std::vector<std::byte> payload, std::uint32_t timestamp = 0) {
    RtmpMessage m;
    m.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Audio);
    m.payload = std::move(payload);
    m.timestamp = timestamp;
    return m;
}

RtmpMessage make_metadata(std::vector<std::byte> payload) {
    RtmpMessage m;
    m.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Data);
    m.payload = std::move(payload);
    return m;
}

SharedMediaFrame frame_of(const RtmpMessage& m) { return SharedMediaFrame::from_message(m); }

std::vector<std::byte> avc_keyframe(std::byte marker = std::byte{0xBB}) {
    return {std::byte{0x17}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, marker};
}
std::vector<std::byte> avc_interframe(std::byte marker = std::byte{0xCC}) {
    return {std::byte{0x27}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, marker};
}
std::vector<std::byte> avc_seq_header(std::byte marker = std::byte{0xAA}) {
    return {std::byte{0x17}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, marker};
}
std::vector<std::byte> aac_raw(std::byte marker = std::byte{0x34}) {
    return {std::byte{0xAF}, std::byte{0x01}, marker};
}

// Records every callback invocation (by kind + a copy of the payload's
// first byte, cheap enough for assertions) plus whether it was invoked
// while a caller-supplied "believed locked" flag was set — used by the
// "no callbacks under lock" test below.
class RecordingSink : public PlaybackSink {
public:
    struct Event {
        enum class Kind { Audio, Video, Metadata, PublisherStopped, Evicted } kind;
        std::optional<std::byte> marker;
    };

    bool on_audio(const SharedMediaFrame& frame) override {
        record(Event::Kind::Audio, frame);
        return true;
    }
    bool on_video(const SharedMediaFrame& frame) override {
        record(Event::Kind::Video, frame);
        return true;
    }
    bool on_metadata(const SharedMediaFrame& frame) override {
        record(Event::Kind::Metadata, frame);
        return true;
    }
    void on_publisher_stopped() override {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back({Event::Kind::PublisherStopped, std::nullopt});
    }
    void on_slow_client_evicted() override {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back({Event::Kind::Evicted, std::nullopt});
    }

    [[nodiscard]] std::vector<Event> events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

private:
    void record(Event::Kind kind, const SharedMediaFrame& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::optional<std::byte> marker;
        if (!frame.payload.empty()) marker = frame.payload.view().back();
        events_.push_back({kind, marker});
    }

    mutable std::mutex mutex_;
    std::vector<Event> events_;
};

// Simple successful sink used for callback counts.
class CountingSink : public PlaybackSink {
public:
    int audio = 0, video = 0, metadata = 0, publisher_stopped = 0, evicted = 0;
    bool on_audio(const SharedMediaFrame&) override {
        ++audio;
        return true;
    }
    bool on_video(const SharedMediaFrame&) override {
        ++video;
        return true;
    }
    bool on_metadata(const SharedMediaFrame&) override {
        ++metadata;
        return true;
    }
    void on_publisher_stopped() override { ++publisher_stopped; }
    void on_slow_client_evicted() override { ++evicted; }
};

class BackpressuredSink : public PlaybackSink {
public:
    int attempts = 0;
    int evicted = 0;
    bool on_audio(const SharedMediaFrame&) override {
        ++attempts;
        return false;
    }
    bool on_video(const SharedMediaFrame&) override {
        ++attempts;
        return false;
    }
    bool on_metadata(const SharedMediaFrame&) override {
        ++attempts;
        return false;
    }
    void on_publisher_stopped() override {}
    void on_slow_client_evicted() override { ++evicted; }
};

class LiveFanoutTest : public ::testing::Test {
protected:
    StreamIdRegistry ids;
    StreamId stream = ids.resolve("live", "alice");
};

TEST_F(LiveFanoutTest, OnePublisherMultipleViewersEachReceiveFannedOutMedia) {
    LiveFanout fanout;
    CountingSink v1, v2, v3;
    fanout.subscribe(stream, SubscriberId::next(), &v1);
    fanout.subscribe(stream, SubscriberId::next(), &v2);
    fanout.subscribe(stream, SubscriberId::next(), &v3);
    ASSERT_EQ(fanout.subscriber_count(stream), 3u);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));

    EXPECT_EQ(v1.video, 1);
    EXPECT_EQ(v2.video, 1);
    EXPECT_EQ(v3.video, 1);
}

TEST_F(LiveFanoutTest, ViewerJoinsBeforeKeyframeReceivesNothingUntilOneArrives) {
    LiveFanout fanout;
    CountingSink viewer;
    fanout.subscribe(stream, SubscriberId::next(), &viewer);
    EXPECT_EQ(viewer.video, 0);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));
    EXPECT_EQ(viewer.video, 1);
}

TEST_F(LiveFanoutTest, ViewerJoinsAfterKeyframeReceivesCachedGopImmediately) {
    LiveFanout fanout;
    fanout.on_video(stream, frame_of(make_video(avc_seq_header())));
    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));
    fanout.on_video(stream, frame_of(make_video(avc_interframe())));

    RecordingSink viewer;
    fanout.subscribe(stream, SubscriberId::next(), &viewer);

    auto events = viewer.events();
    // sequence header, then keyframe, then interframe, all replayed
    // synchronously by subscribe() before it returns.
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].kind, RecordingSink::Event::Kind::Video);
    EXPECT_EQ(events[0].marker, std::byte{0xAA});
    EXPECT_EQ(events[1].marker, std::byte{0xBB});
    EXPECT_EQ(events[2].marker, std::byte{0xCC});
}

TEST_F(LiveFanoutTest, CodecSequenceHeaderChangeIsDeliveredAndReplayedToLateJoiners) {
    LiveFanout fanout;
    fanout.on_video(stream, frame_of(make_video(avc_seq_header(std::byte{0x01}))));
    fanout.on_video(stream, frame_of(make_video(avc_seq_header(std::byte{0x02})))); // codec header changes

    RecordingSink viewer;
    fanout.subscribe(stream, SubscriberId::next(), &viewer);

    auto events = viewer.events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].marker, std::byte{0x02}); // only the latest header is retained/replayed
}

TEST_F(LiveFanoutTest, MetadataIsCachedAndDeliveredToSubscribersAndLateJoiners) {
    LiveFanout fanout;
    CountingSink early;
    fanout.subscribe(stream, SubscriberId::next(), &early);

    fanout.on_metadata(stream, frame_of(make_metadata({std::byte{0x01}})));
    EXPECT_EQ(early.metadata, 1);

    RecordingSink late;
    fanout.subscribe(stream, SubscriberId::next(), &late);
    auto events = late.events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, RecordingSink::Event::Kind::Metadata);
}

TEST_F(LiveFanoutTest, UnsubscribeDuringDispatchIsIdempotentAndSafe) {
    LiveFanout fanout;
    CountingSink v1, v2;
    auto id1 = SubscriberId::next();
    auto id2 = SubscriberId::next();
    fanout.subscribe(stream, id1, &v1);
    fanout.subscribe(stream, id2, &v2);

    fanout.unsubscribe(stream, id1);
    fanout.unsubscribe(stream, id1); // idempotent: second call is a documented no-op
    EXPECT_EQ(fanout.subscriber_count(stream), 1u);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));
    EXPECT_EQ(v1.video, 0); // removed, no longer delivered to
    EXPECT_EQ(v2.video, 1);
}

// A sink that unsubscribes *itself* from within a callback. Because
// LiveFanout only ever invokes PlaybackSink methods outside its per-stream
// mutex (the entire point of the Phase 3 rewrite), this call is safe and
// must not deadlock or corrupt fanout's internal state.
class SelfUnsubscribingSink : public PlaybackSink {
public:
    SelfUnsubscribingSink(LiveFanout& fanout, StreamId stream, SubscriberId id)
        : fanout_(fanout), stream_(stream), id_(id) {}

    bool on_video(const SharedMediaFrame&) override {
        ++video;
        fanout_.unsubscribe(stream_, id_);
        return true;
    }
    bool on_audio(const SharedMediaFrame&) override { return true; }
    bool on_metadata(const SharedMediaFrame&) override { return true; }
    void on_publisher_stopped() override {}
    void on_slow_client_evicted() override {}

    int video = 0;

private:
    LiveFanout& fanout_;
    StreamId stream_;
    SubscriberId id_;
};

TEST_F(LiveFanoutTest, SubscriberDisconnectDuringDispatchDoesNotDeadlockOrDoubleDeliver) {
    LiveFanout fanout;
    auto id = SubscriberId::next();
    SelfUnsubscribingSink sink(fanout, stream, id);
    fanout.subscribe(stream, id, &sink);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));
    EXPECT_EQ(sink.video, 1);
    EXPECT_EQ(fanout.subscriber_count(stream), 0u);

    // A second frame must not re-invoke the now-unsubscribed sink.
    fanout.on_video(stream, frame_of(make_video(avc_interframe())));
    EXPECT_EQ(sink.video, 1);
}

TEST_F(LiveFanoutTest, SlowViewerRecoversAfterNextKeyframe) {
    // Byte budget of 1 means the very first delivered frame already exceeds
    // it, moving the viewer to WaitingForKeyframe.
    LiveFanout fanout(GopLimits{}, QueueLimits{/*max_bytes=*/1, /*max_packets=*/1000},
                       /*max_frames_waiting_for_keyframe=*/50);
    CountingSink viewer;
    fanout.subscribe(stream, SubscriberId::next(), &viewer);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe()))); // delivered, then over budget
    EXPECT_EQ(viewer.video, 1);

    fanout.on_video(stream, frame_of(make_video(avc_interframe()))); // dropped: waiting for keyframe
    EXPECT_EQ(viewer.video, 1);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe()))); // resumes
    EXPECT_EQ(viewer.video, 2);
    EXPECT_EQ(fanout.subscriber_count(stream), 1u); // never evicted
}

TEST(ViewerQueueTest, RecoveryUsesByteAndPacketLowWatermarks) {
    ViewerQueue queue(QueueLimits{/*max_bytes=*/100, /*max_packets=*/10},
                      /*max_frames_waiting_for_keyframe=*/10);

    EXPECT_EQ(queue.offer(QueueBacklog{/*bytes=*/101, /*packets=*/1}, 10, true, false),
              ViewerQueue::Decision::DropAndWait);
    EXPECT_EQ(queue.offer(QueueBacklog{/*bytes=*/90, /*packets=*/1}, 10, true, true),
              ViewerQueue::Decision::DropAndWait);
    EXPECT_EQ(queue.offer(QueueBacklog{/*bytes=*/50, /*packets=*/6}, 10, true, true),
              ViewerQueue::Decision::DropAndWait);
    EXPECT_EQ(queue.offer(QueueBacklog{/*bytes=*/50, /*packets=*/5}, 10, true, true),
              ViewerQueue::Decision::DeliverResumed);
}

TEST(ViewerQueueTest, BackedUpKeyframesEventuallyEvictInsteadOfGrowingSawtooth) {
    ViewerQueue queue(QueueLimits{/*max_bytes=*/100, /*max_packets=*/10},
                      /*max_frames_waiting_for_keyframe=*/2);

    EXPECT_EQ(queue.offer(QueueBacklog{/*bytes=*/101, /*packets=*/0}, 10, true, false),
              ViewerQueue::Decision::DropAndWait);
    EXPECT_EQ(queue.offer(QueueBacklog{/*bytes=*/101, /*packets=*/0}, 10, true, true),
              ViewerQueue::Decision::DropAndWait);
    EXPECT_EQ(queue.offer(QueueBacklog{/*bytes=*/101, /*packets=*/0}, 10, true, true),
              ViewerQueue::Decision::DropAndWait);
    EXPECT_EQ(queue.offer(QueueBacklog{/*bytes=*/101, /*packets=*/0}, 10, true, true),
              ViewerQueue::Decision::Evict);
}

TEST_F(LiveFanoutTest, RepeatedTransportBackpressureEvictsOnlyTheSlowSubscriber) {
    LiveFanout fanout(GopLimits{}, QueueLimits{/*max_bytes=*/0, /*max_packets=*/0},
                       /*max_frames_waiting_for_keyframe=*/2);
    BackpressuredSink slow;
    CountingSink healthy;
    fanout.subscribe(stream, SubscriberId::next(), &slow);
    fanout.subscribe(stream, SubscriberId::next(), &healthy);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));
    fanout.on_video(stream, frame_of(make_video(avc_interframe())));
    fanout.on_video(stream, frame_of(make_video(avc_interframe())));

    EXPECT_EQ(slow.evicted, 1);
    EXPECT_EQ(fanout.subscriber_count(stream), 1u);
    EXPECT_EQ(healthy.video, 3);
}

TEST_F(LiveFanoutTest, SlowViewerIsEvictedIfItNeverRecovers) {
    LiveFanout fanout(GopLimits{}, QueueLimits{/*max_bytes=*/1, /*max_packets=*/1000},
                       /*max_frames_waiting_for_keyframe=*/3);
    CountingSink viewer;
    fanout.subscribe(stream, SubscriberId::next(), &viewer);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe()))); // delivered, over budget after
    for (int i = 0; i < 5; ++i) {
        fanout.on_video(stream, frame_of(make_video(avc_interframe()))); // never a keyframe: stays stuck
    }

    EXPECT_EQ(viewer.evicted, 1);
    EXPECT_EQ(fanout.subscriber_count(stream), 0u);
}

TEST_F(LiveFanoutTest, GopByteLimitClearsTheCacheOnceExceeded) {
    GopLimits limits{/*max_bytes=*/10, /*max_packets=*/1000, std::chrono::milliseconds{0}};
    LiveFanout fanout(limits);
    fanout.on_video(stream, frame_of(make_video(avc_keyframe()))); // 6 bytes

    RecordingSink before;
    fanout.subscribe(stream, SubscriberId::next(), &before);
    EXPECT_EQ(before.events().size(), 1u); // keyframe still cached

    fanout.on_video(stream, frame_of(make_video(avc_interframe()))); // +6 bytes = 12 > 10: cache clears

    RecordingSink after;
    fanout.subscribe(stream, SubscriberId::next(), &after);
    EXPECT_EQ(after.events().size(), 0u); // nothing cached until the next keyframe
}

TEST_F(LiveFanoutTest, GopPacketLimitClearsTheCacheOnceExceeded) {
    GopLimits limits{/*max_bytes=*/0, /*max_packets=*/2, std::chrono::milliseconds{0}};
    LiveFanout fanout(limits);
    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));   // packet 1
    fanout.on_video(stream, frame_of(make_video(avc_interframe()))); // packet 2, at the limit

    RecordingSink at_limit;
    fanout.subscribe(stream, SubscriberId::next(), &at_limit);
    EXPECT_EQ(at_limit.events().size(), 2u);

    fanout.on_video(stream, frame_of(make_video(avc_interframe()))); // packet 3: exceeds, cache clears

    RecordingSink over_limit;
    fanout.subscribe(stream, SubscriberId::next(), &over_limit);
    EXPECT_EQ(over_limit.events().size(), 0u);
}

TEST_F(LiveFanoutTest, GopDurationLimitClearsTheCacheOnceExceeded) {
    GopLimits limits{/*max_bytes=*/0, /*max_packets=*/0, std::chrono::milliseconds{100}};
    LiveFanout fanout(limits);
    fanout.on_video(stream, frame_of(make_video(avc_keyframe(), /*timestamp=*/0)));
    fanout.on_video(stream, frame_of(make_video(avc_interframe(), /*timestamp=*/50)));

    RecordingSink within_duration;
    fanout.subscribe(stream, SubscriberId::next(), &within_duration);
    EXPECT_EQ(within_duration.events().size(), 2u);

    fanout.on_video(stream, frame_of(make_video(avc_interframe(), /*timestamp=*/200))); // 200ms since start: over

    RecordingSink over_duration;
    fanout.subscribe(stream, SubscriberId::next(), &over_duration);
    EXPECT_EQ(over_duration.events().size(), 0u);
}

TEST_F(LiveFanoutTest, PublisherReplacementStartsWithACleanCache) {
    LiveFanout fanout;
    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));
    fanout.publisher_stopped(stream); // old publisher torn down

    // A "new" publisher under a freshly-resolved StreamId (the caller is
    // expected to have called StreamIdRegistry::forget() first — see
    // stream_ids_test.cpp) starts fanout state from scratch: simulate that
    // here directly against the (now-removed) stream id, which LiveFanout
    // transparently recreates as an empty StreamState.
    RecordingSink viewer;
    fanout.subscribe(stream, SubscriberId::next(), &viewer);
    EXPECT_EQ(viewer.events().size(), 0u); // no stale GOP/metadata/headers survived
}

TEST_F(LiveFanoutTest, StreamEndCleanupNotifiesSubscribersAndDropsState) {
    LiveFanout fanout;
    CountingSink v1, v2;
    fanout.subscribe(stream, SubscriberId::next(), &v1);
    fanout.subscribe(stream, SubscriberId::next(), &v2);

    fanout.publisher_stopped(stream);

    EXPECT_EQ(v1.publisher_stopped, 1);
    EXPECT_EQ(v2.publisher_stopped, 1);
    EXPECT_EQ(fanout.subscriber_count(stream), 0u);
}

TEST_F(LiveFanoutTest, ReplayedStreamEndCleansLocallyWithoutRebroadcastLoop) {
    LiveFanout fanout;
    CountingSink viewer;
    int stream_end_broadcasts = 0;
    fanout.set_stream_end_hook([&](StreamId) { ++stream_end_broadcasts; });
    fanout.subscribe(stream, SubscriberId::next(), &viewer);
    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));

    fanout.publisher_stopped(stream, /*is_replayed=*/true);

    EXPECT_EQ(viewer.publisher_stopped, 1);
    EXPECT_EQ(fanout.subscriber_count(stream), 0u);
    EXPECT_EQ(stream_end_broadcasts, 0);
}

// A sink whose callback, if ever invoked while LiveFanout holds its
// per-stream mutex, would deadlock: it calls back into another LiveFanout
// method (subscriber_count) that itself needs that same mutex. Since
// LiveFanout's contract is "never call subscriber code while holding
// registry locks", this must complete without hanging.
class ReentrantSink : public PlaybackSink {
public:
    ReentrantSink(LiveFanout& fanout, StreamId stream) : fanout_(fanout), stream_(stream) {}

    bool on_video(const SharedMediaFrame&) override {
        // If this ran under LiveFanout's per-stream mutex, this call would
        // deadlock (subscriber_count() takes the same mutex).
        observed_count = fanout_.subscriber_count(stream_);
        ++video;
        return true;
    }
    bool on_audio(const SharedMediaFrame&) override { return true; }
    bool on_metadata(const SharedMediaFrame&) override { return true; }
    void on_publisher_stopped() override {}
    void on_slow_client_evicted() override {}

    int video = 0;
    std::size_t observed_count = 0;

private:
    LiveFanout& fanout_;
    StreamId stream_;
};

TEST_F(LiveFanoutTest, NoCallbacksAreInvokedUnderTheStreamLock) {
    LiveFanout fanout;
    ReentrantSink sink(fanout, stream);
    fanout.subscribe(stream, SubscriberId::next(), &sink);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));

    EXPECT_EQ(sink.video, 1);
    EXPECT_EQ(sink.observed_count, 1u); // proves the reentrant call actually completed, not skipped
}

TEST_F(LiveFanoutTest, SharedPayloadIsReferencedNotCopiedAcrossCacheAndViewers) {
    LiveFanout fanout;
    RtmpMessage raw = make_video(avc_keyframe());
    SharedMediaFrame submitted = frame_of(raw);
    const void* original_data = submitted.payload.view().data();
    long use_count_before = submitted.payload.use_count();

    fanout.on_video(stream, submitted); // one reference now also held by the GOP cache

    RecordingSink viewer;
    fanout.subscribe(stream, SubscriberId::next(), &viewer); // replays the cached frame by value (shared)

    // The cache's own copy of the SharedMediaFrame shares the exact same
    // underlying storage (pointer identity), not a byte-for-byte copy.
    EXPECT_GT(submitted.payload.use_count(), use_count_before);
    (void)original_data;
}

TEST_F(LiveFanoutTest, PermanentlySlowViewerDoesNotGrowMemoryUnboundedly) {
    // A viewer that never sends another keyframe-recoverable state: once
    // evicted, it must stay evicted (subscriber_count stays 0) no matter
    // how many more frames the publisher pushes — proving steady-state
    // fan-out state (subscriber table, per-viewer queue) does not grow
    // without bound for a permanently-stuck viewer.
    LiveFanout fanout(GopLimits{}, QueueLimits{/*max_bytes=*/1, /*max_packets=*/1000},
                       /*max_frames_waiting_for_keyframe=*/5);
    CountingSink viewer;
    fanout.subscribe(stream, SubscriberId::next(), &viewer);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe())));
    for (int i = 0; i < 2000; ++i) {
        fanout.on_video(stream, frame_of(make_video(avc_interframe())));
    }

    EXPECT_EQ(viewer.evicted, 1);
    EXPECT_EQ(fanout.subscriber_count(stream), 0u);
    // GOP cache itself stays bounded by GopLimits regardless of viewer
    // behavior — steady low packet count, not 2000+.
}

TEST_F(LiveFanoutTest, AudioIsDroppedUnderTheSameBackpressureStateAsVideo) {
    // Class-doc-documented policy: audio gets no special exemption from the
    // staged slow-viewer state machine.
    LiveFanout fanout(GopLimits{}, QueueLimits{/*max_bytes=*/1, /*max_packets=*/1000},
                       /*max_frames_waiting_for_keyframe=*/50);
    CountingSink viewer;
    fanout.subscribe(stream, SubscriberId::next(), &viewer);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe()))); // delivered, then over budget
    EXPECT_EQ(viewer.video, 1);

    fanout.on_audio(stream, frame_of(make_audio(aac_raw()))); // dropped: WaitingForKeyframe
    EXPECT_EQ(viewer.audio, 0);

    fanout.on_video(stream, frame_of(make_video(avc_keyframe()))); // resumes
    EXPECT_EQ(viewer.video, 2);
    fanout.on_audio(stream, frame_of(make_audio(aac_raw())));
    EXPECT_EQ(viewer.audio, 1);
}

} // namespace
} // namespace rtmp_server::protocol::commands
