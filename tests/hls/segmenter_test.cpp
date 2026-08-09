#include <gtest/gtest.h>

#include <vector>

#include "rtmp_server/hls/segmenter.hpp"
#include "rtmp_server/media/ts/ts_muxer.hpp"
#include "test_media.hpp"

using namespace rtmp_server;
using namespace rtmp_server::hls;
using namespace rtmp_server::hls_test;
using namespace std::chrono_literals;

namespace {

// Collects everything the segmenter emits.
class Collector {
public:
    Segmenter::SegmentCallback callback() {
        return [this](SegmentPtr segment) { segments_.push_back(std::move(segment)); };
    }
    [[nodiscard]] const std::vector<SegmentPtr>& segments() const { return segments_; }
    [[nodiscard]] std::size_t count() const { return segments_.size(); }

private:
    std::vector<SegmentPtr> segments_;
};

// Feeds one GOP: a keyframe followed by `inter_frames` inter frames, plus an
// audio frame each, advancing the timestamp by `step_ms` per frame.
void feed_gop(Segmenter& segmenter, std::uint32_t& timestamp, int inter_frames = 4,
              std::uint32_t step_ms = 200) {
    segmenter.on_video(video_message(timestamp, avc_frame(/*keyframe=*/true, 128)));
    segmenter.on_audio(audio_message(timestamp, aac_frame()));
    timestamp += step_ms;
    for (int i = 0; i < inter_frames; ++i) {
        segmenter.on_video(video_message(timestamp, avc_frame(false, 128)));
        segmenter.on_audio(audio_message(timestamp, aac_frame()));
        timestamp += step_ms;
    }
}

void send_headers(Segmenter& segmenter) {
    segmenter.on_video(video_message(0, avc_sequence_header()));
    segmenter.on_audio(audio_message(0, aac_sequence_header()));
}

bool looks_like_ts(const SegmentPtr& segment) {
    const auto view = segment->data.view();
    if (view.empty() || view.size() % media::ts::kPacketSize != 0) return false;
    for (std::size_t i = 0; i < view.size(); i += media::ts::kPacketSize) {
        if (view[i] != std::byte{0x47}) return false;
    }
    return true;
}

} // namespace

TEST(SegmenterTest, ProducesSegmentsCutOnKeyframesAtTheTargetDuration) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 1000ms;
    Segmenter segmenter(collector.callback(), config);

    send_headers(segmenter);
    EXPECT_TRUE(segmenter.has_video_config());
    EXPECT_TRUE(segmenter.has_audio_config());

    std::uint32_t timestamp = 0;
    for (int gop = 0; gop < 5; ++gop) feed_gop(segmenter, timestamp);
    segmenter.finalize();

    EXPECT_GE(collector.count(), 3u);
    for (const auto& segment : collector.segments()) {
        EXPECT_TRUE(looks_like_ts(segment)) << "segment " << segment->name << " is not valid TS";
        EXPECT_GT(segment->size_bytes(), 0u);
    }
}

TEST(SegmenterTest, EverySegmentBeginsWithPatAndPmtSoItIsIndependentlyDecodable) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    for (int gop = 0; gop < 4; ++gop) feed_gop(segmenter, timestamp);
    segmenter.finalize();

    ASSERT_GE(collector.count(), 2u);
    for (const auto& segment : collector.segments()) {
        const auto view = segment->data.view();
        ASSERT_GE(view.size(), 2 * media::ts::kPacketSize);
        // Packet 0 = PAT (PID 0), packet 1 = PMT.
        const unsigned pid0 = ((static_cast<unsigned>(view[1]) & 0x1F) << 8) |
                              static_cast<unsigned>(view[2]);
        EXPECT_EQ(pid0, 0u);
        const std::size_t p1 = media::ts::kPacketSize;
        const unsigned pid1 = ((static_cast<unsigned>(view[p1 + 1]) & 0x1F) << 8) |
                              static_cast<unsigned>(view[p1 + 2]);
        EXPECT_EQ(pid1, 0x1000u);
    }
}

TEST(SegmenterTest, SegmentsAreSequentiallyNumberedAndNamed) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    for (int gop = 0; gop < 5; ++gop) feed_gop(segmenter, timestamp);
    segmenter.finalize();

    ASSERT_GE(collector.count(), 2u);
    for (std::size_t i = 0; i < collector.count(); ++i) {
        EXPECT_EQ(collector.segments()[i]->sequence, i);
        EXPECT_EQ(collector.segments()[i]->name, "segment-" + std::to_string(i) + ".ts");
    }
}

TEST(SegmenterTest, ResumedSegmenterStartsAtConfiguredSequence) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    config.initial_sequence = 42;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    for (int gop = 0; gop < 3; ++gop) feed_gop(segmenter, timestamp);
    segmenter.finalize();

    ASSERT_FALSE(collector.segments().empty());
    EXPECT_EQ(collector.segments().front()->sequence, 42u);
    EXPECT_EQ(collector.segments().front()->name, "segment-42.ts");
}

TEST(SegmenterTest, MediaBeforeTheSequenceHeaderIsDroppedNotPackaged) {
    Collector collector;
    Segmenter segmenter(collector.callback(), {});

    // No SPS/PPS yet: a segment starting here would be undecodable.
    segmenter.on_video(video_message(0, avc_frame(true, 128)));
    segmenter.on_audio(audio_message(0, aac_frame()));
    EXPECT_EQ(collector.count(), 0u);
    EXPECT_GT(segmenter.stats().dropped_frames, 0u);
    EXPECT_EQ(segmenter.stats().video_frames, 0u);
}

TEST(SegmenterTest, AudioBeforeTheFirstKeyframeIsDropped) {
    Collector collector;
    Segmenter segmenter(collector.callback(), {});
    send_headers(segmenter);

    // Segments must start on a video keyframe; audio alone must not open one.
    segmenter.on_audio(audio_message(0, aac_frame()));
    EXPECT_EQ(segmenter.stats().audio_frames, 0u);
    EXPECT_GT(segmenter.stats().dropped_frames, 0u);
}

// --- Sequence header changes ----------------------------------------------

TEST(SegmenterTest, VideoSequenceHeaderChangeCutsASegmentAndMarksDiscontinuity) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    feed_gop(segmenter, timestamp);
    feed_gop(segmenter, timestamp);

    const auto before = collector.count();

    // A genuinely different SPS (level 0x28 rather than 0x1E).
    auto changed_sps = sps_nal();
    changed_sps[3] = std::byte{0x28};
    segmenter.on_video(video_message(timestamp, avc_sequence_header(changed_sps, pps_nal())));
    EXPECT_EQ(segmenter.stats().sequence_header_changes, 1u);
    EXPECT_GT(collector.count(), before) << "the open segment must be closed on a codec change";

    feed_gop(segmenter, timestamp);
    segmenter.finalize();

    // Exactly one segment after the change carries the discontinuity flag.
    std::size_t flagged = 0;
    for (const auto& segment : collector.segments()) {
        if (segment->discontinuity) ++flagged;
    }
    EXPECT_EQ(flagged, 1u);
    EXPECT_EQ(segmenter.stats().discontinuities, 1u);
}

TEST(SegmenterTest, RepeatingTheIdenticalSequenceHeaderIsNotADiscontinuity) {
    Collector collector;
    Segmenter segmenter(collector.callback(), {});
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    feed_gop(segmenter, timestamp);
    // Encoders resend the same config periodically; that is not a change.
    segmenter.on_video(video_message(timestamp, avc_sequence_header()));
    segmenter.on_audio(audio_message(timestamp, aac_sequence_header()));
    feed_gop(segmenter, timestamp);
    segmenter.finalize();

    EXPECT_EQ(segmenter.stats().sequence_header_changes, 0u);
    EXPECT_EQ(segmenter.stats().discontinuities, 0u);
}

TEST(SegmenterTest, AudioSequenceHeaderChangeIsADiscontinuity) {
    Collector collector;
    Segmenter segmenter(collector.callback(), {});
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    feed_gop(segmenter, timestamp);
    // 44100 stereo -> 48000 mono.
    segmenter.on_audio(audio_message(timestamp, aac_sequence_header(3, 1)));
    EXPECT_EQ(segmenter.stats().sequence_header_changes, 1u);
    feed_gop(segmenter, timestamp);
    segmenter.finalize();
    EXPECT_EQ(segmenter.stats().discontinuities, 1u);
}

TEST(SegmenterTest, MalformedSequenceHeaderIsRejectedWithoutChangingState) {
    Collector collector;
    Segmenter segmenter(collector.callback(), {});
    send_headers(segmenter);

    std::vector<std::byte> bad;
    append(bad, {0x17, 0x00, 0x00, 0x00, 0x00, 0x01, 0x42}); // truncated config record
    segmenter.on_video(video_message(100, bad));

    EXPECT_TRUE(segmenter.has_video_config()); // the good config is retained
    EXPECT_EQ(segmenter.stats().sequence_header_changes, 0u);
    EXPECT_GT(segmenter.stats().dropped_frames, 0u);
}

// --- Timestamp discontinuity ----------------------------------------------

TEST(SegmenterTest, LargeBackwardTimestampJumpIsTreatedAsADiscontinuity) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    config.discontinuity_threshold = 5000ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 100000;
    feed_gop(segmenter, timestamp);
    feed_gop(segmenter, timestamp);

    // A 32-bit rollover / encoder restart: time jumps back to near zero.
    std::uint32_t restarted = 10;
    feed_gop(segmenter, restarted);
    segmenter.finalize();

    EXPECT_GE(segmenter.stats().discontinuities, 1u);
    bool any_flagged = false;
    for (const auto& segment : collector.segments()) {
        if (segment->discontinuity) any_flagged = true;
    }
    EXPECT_TRUE(any_flagged);
}

TEST(SegmenterTest, LargeForwardTimestampGapIsAlsoADiscontinuity) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    config.discontinuity_threshold = 5000ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    feed_gop(segmenter, timestamp);
    timestamp += 120000; // a two-minute stall
    feed_gop(segmenter, timestamp);
    segmenter.finalize();

    EXPECT_GE(segmenter.stats().discontinuities, 1u);
}

TEST(SegmenterTest, SmallTimestampJitterIsNotADiscontinuity) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    config.discontinuity_threshold = 5000ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 1000;
    feed_gop(segmenter, timestamp);
    // Normal inter-frame jitter must not trigger a decoder reset.
    segmenter.on_video(video_message(timestamp - 30, avc_frame(false, 64)));
    feed_gop(segmenter, timestamp);
    segmenter.finalize();

    EXPECT_EQ(segmenter.stats().discontinuities, 0u);
}

TEST(SegmenterTest, PublisherReconnectFlushesAndMarksADiscontinuity) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    feed_gop(segmenter, timestamp);
    feed_gop(segmenter, timestamp);

    segmenter.mark_publisher_reconnect();
    // A reconnect drops the cached codec configs; the new publisher must
    // resend them before any media is packaged again.
    EXPECT_FALSE(segmenter.has_video_config());

    send_headers(segmenter);
    std::uint32_t restarted = 0;
    feed_gop(segmenter, restarted);
    feed_gop(segmenter, restarted);
    segmenter.finalize();

    EXPECT_GE(segmenter.stats().discontinuities, 1u);
    EXPECT_GE(collector.count(), 2u);
}

TEST(SegmenterTest, TranscodedMediaGapDoesNotApplyASecondTimestampOffset) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 10'000;
    feed_gop(segmenter, timestamp);
    feed_gop(segmenter, timestamp);

    segmenter.mark_media_discontinuity();
    EXPECT_FALSE(segmenter.has_video_config());

    // A transcoder-generated stream continues its clock across the gap. It
    // still resends codec headers on the next IDR, but needs no timeline-base
    // adjustment intended for RTMP publishers that restart from timestamp 0.
    send_headers(segmenter);
    feed_gop(segmenter, timestamp);
    feed_gop(segmenter, timestamp);
    segmenter.finalize();

    EXPECT_GE(segmenter.stats().discontinuities, 1u);
    EXPECT_GE(collector.count(), 2u);
}

// --- Bounds ---------------------------------------------------------------

TEST(SegmenterTest, SegmentIsForceCutWhenTheByteBoundIsExceeded) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 600000ms;      // effectively never cut on time
    config.max_segment_duration = 600000ms;
    config.max_segment_bytes = 32 * 1024;   // but cut on size
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    // A single enormous GOP: without the byte bound this would buffer forever.
    segmenter.on_video(video_message(timestamp, avc_frame(true, 4096)));
    timestamp += 40;
    for (int i = 0; i < 60; ++i) {
        segmenter.on_video(video_message(timestamp, avc_frame(false, 4096)));
        timestamp += 40;
    }

    EXPECT_GT(collector.count(), 0u) << "the byte bound must force a cut";
    EXPECT_GT(segmenter.stats().forced_cuts, 0u);
    for (const auto& segment : collector.segments()) {
        // Each segment stays near the bound (one access unit may overshoot).
        EXPECT_LT(segment->size_bytes(), config.max_segment_bytes * 3);
    }
}

TEST(SegmenterTest, SegmentIsForceCutWhenTheDurationBoundIsExceeded) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 600000ms;
    config.max_segment_duration = 2000ms;
    config.max_segment_bytes = 64 * 1024 * 1024;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    segmenter.on_video(video_message(timestamp, avc_frame(true, 64)));
    for (int i = 0; i < 30; ++i) {
        timestamp += 200;
        segmenter.on_video(video_message(timestamp, avc_frame(false, 64)));
    }

    EXPECT_GT(collector.count(), 0u);
    EXPECT_GT(segmenter.stats().forced_cuts, 0u);
}

TEST(SegmenterTest, ForcedCutsAreMarkedDiscontinuousBecauseTheyAreNotKeyframeAligned) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 600000ms;
    config.max_segment_duration = 1000ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    segmenter.on_video(video_message(timestamp, avc_frame(true, 64)));
    for (int i = 0; i < 20; ++i) {
        timestamp += 300;
        segmenter.on_video(video_message(timestamp, avc_frame(false, 64)));
    }
    segmenter.finalize();

    ASSERT_GE(collector.count(), 2u);
    // Every segment after the first forced cut is flagged.
    bool saw_flag = false;
    for (std::size_t i = 1; i < collector.count(); ++i) {
        if (collector.segments()[i]->discontinuity) saw_flag = true;
    }
    EXPECT_TRUE(saw_flag);
}

// --- Lifecycle ------------------------------------------------------------

TEST(SegmenterTest, FinalizePublishesTheTrailingPartialSegment) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 60000ms; // no natural cut will occur
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    feed_gop(segmenter, timestamp);
    EXPECT_EQ(collector.count(), 0u);

    segmenter.finalize();
    EXPECT_EQ(collector.count(), 1u) << "the tail of the stream must not be lost";
}

TEST(SegmenterTest, FinalizeIsIdempotentAndIgnoresLateMedia) {
    Collector collector;
    Segmenter segmenter(collector.callback(), {});
    send_headers(segmenter);
    std::uint32_t timestamp = 0;
    feed_gop(segmenter, timestamp);

    segmenter.finalize();
    const auto after_first = collector.count();
    segmenter.finalize();
    segmenter.finalize();
    EXPECT_EQ(collector.count(), after_first);

    // Media arriving after finalize is ignored, not packaged.
    segmenter.on_video(video_message(9999, avc_frame(true, 64)));
    EXPECT_EQ(collector.count(), after_first);
}

TEST(SegmenterTest, CodecsAttributeIsDerivedFromTheSpsAndAudioConfig) {
    Collector collector;
    Segmenter segmenter(collector.callback(), {});
    EXPECT_TRUE(segmenter.codecs_attribute().empty());

    send_headers(segmenter);
    const auto codecs = segmenter.codecs_attribute();
    // avc1.<profile><compat><level> from SPS bytes 42 C0 1E, plus AAC-LC.
    EXPECT_EQ(codecs, "avc1.42C01E,mp4a.40.2");
}

TEST(SegmenterTest, SegmentBytesAreSharedNotCopiedPerConsumer) {
    Collector collector;
    SegmenterConfig config;
    config.target_duration = 500ms;
    Segmenter segmenter(collector.callback(), config);
    send_headers(segmenter);

    std::uint32_t timestamp = 0;
    for (int gop = 0; gop < 3; ++gop) feed_gop(segmenter, timestamp);
    segmenter.finalize();

    ASSERT_GE(collector.count(), 1u);
    const auto& segment = collector.segments().front();
    const auto* original = segment->data.view().data();

    // Simulating many concurrent viewers holding the segment: all of them
    // observe the identical buffer address — no deep copy per viewer (3.8).
    std::vector<SegmentPtr> viewers(100, segment);
    for (const auto& v : viewers) {
        EXPECT_EQ(v->data.view().data(), original);
    }
}
