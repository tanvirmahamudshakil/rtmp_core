#include <gtest/gtest.h>

#include <cstring>

#include "dash_test_media.hpp"
#include "rtmp_server/dash/segmenter.hpp"

using namespace rtmp_server;
using namespace rtmp_server::dash;
using namespace rtmp_server::dash_test;

namespace {

std::uint32_t read_u32(std::span<const std::byte> data, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value = (value << 8) | static_cast<std::uint8_t>(data[offset + i]);
    return value;
}

std::string fourcc(std::span<const std::byte> data, std::size_t offset) {
    std::string out;
    for (std::size_t i = 0; i < 4; ++i) out.push_back(static_cast<char>(data[offset + i]));
    return out;
}

// Feeds `count` frames of 10 fps synthetic video, a keyframe every
// `keyframe_every`th frame, into `segmenter`.
void feed_video(Segmenter& segmenter, int count, int keyframe_every, std::uint32_t start_ts = 0) {
    for (int i = 0; i < count; ++i) {
        const std::uint32_t ts = start_ts + static_cast<std::uint32_t>(i) * 100;
        const bool keyframe = (i % keyframe_every) == 0;
        segmenter.on_video(video_message(ts, avc_frame(keyframe)));
    }
}

} // namespace

TEST(DashSegmenterTest, BuildsAnInitSegmentAsEachConfigArrivesAndAgainWhenAudioJoins) {
    std::vector<InitSegmentPtr> inits;
    Segmenter segmenter([](SegmentPtr) {}, [&inits](InitSegmentPtr init) { inits.push_back(init); });

    segmenter.on_video(video_message(0, avc_sequence_header()));
    // A video-only init is published immediately: a player joining before
    // audio config arrives should not have to wait for it to start decoding
    // video, and the vast majority of RTMP publishers send both headers
    // within the same first burst regardless.
    ASSERT_EQ(inits.size(), 1u);
    EXPECT_GT(inits.front()->data.size(), 0u);
    EXPECT_EQ(inits.front()->epoch, 1u);

    segmenter.on_audio(audio_message(0, aac_sequence_header()));
    ASSERT_EQ(inits.size(), 2u);
    EXPECT_EQ(inits.back()->epoch, 2u);
    EXPECT_TRUE(segmenter.has_video_config());
    EXPECT_TRUE(segmenter.has_audio_config());
}

TEST(DashSegmenterTest, ProducesSegmentsCutOnKeyframesAtTheTargetDuration) {
    std::vector<SegmentPtr> segments;
    SegmenterConfig config;
    config.target_duration = std::chrono::milliseconds(2000);
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); }, [](InitSegmentPtr) {},
                        config);

    segmenter.on_video(video_message(0, avc_sequence_header()));
    segmenter.on_audio(audio_message(0, aac_sequence_header()));
    // 4s of 10fps video, keyframe every 2s (every 20th frame).
    feed_video(segmenter, 41, 20);
    segmenter.finalize();

    ASSERT_GE(segments.size(), 2u);
    // Video-only header -> epoch 1, then audio joins -> epoch 2. Both happen
    // before any segment is cut, so every segment here belongs to epoch 2.
    for (const auto& segment : segments) {
        EXPECT_GT(segment->size_bytes(), 0u);
        EXPECT_EQ(segment->init_epoch, 2u);
    }
    // Sequence numbers are strictly increasing and named consistently.
    for (std::size_t i = 0; i < segments.size(); ++i) {
        EXPECT_EQ(segments[i]->number, i);
        EXPECT_EQ(segments[i]->name, "chunk-" + std::to_string(i) + ".m4s");
    }
}

TEST(DashSegmenterTest, MediaBeforeTheSequenceHeaderIsDropped) {
    std::vector<SegmentPtr> segments;
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); }, [](InitSegmentPtr) {});
    segmenter.on_video(video_message(0, avc_frame(true)));
    segmenter.finalize();
    EXPECT_TRUE(segments.empty());
    EXPECT_EQ(segmenter.stats().dropped_frames, 1u);
}

TEST(DashSegmenterTest, AudioBeforeTheFirstKeyframeIsDropped) {
    Segmenter segmenter([](SegmentPtr) {}, [](InitSegmentPtr) {});
    segmenter.on_video(video_message(0, avc_sequence_header()));
    segmenter.on_audio(audio_message(0, aac_sequence_header()));
    segmenter.on_audio(audio_message(0, aac_frame()));
    EXPECT_EQ(segmenter.stats().dropped_frames, 1u);
    EXPECT_EQ(segmenter.stats().audio_frames, 0u);
}

TEST(DashSegmenterTest, ResumedSegmenterStartsAtConfiguredSequence) {
    std::vector<SegmentPtr> segments;
    SegmenterConfig config;
    config.target_duration = std::chrono::milliseconds(500);
    config.initial_sequence = 42;
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); }, [](InitSegmentPtr) {},
                        config);
    segmenter.on_video(video_message(0, avc_sequence_header()));
    segmenter.on_audio(audio_message(0, aac_sequence_header()));
    feed_video(segmenter, 11, 5);
    segmenter.finalize();
    ASSERT_FALSE(segments.empty());
    EXPECT_EQ(segments.front()->number, 42u);
}

TEST(DashSegmenterTest, VideoSequenceHeaderChangeCutsASegmentAndBumpsTheInitEpoch) {
    std::vector<SegmentPtr> segments;
    std::vector<InitSegmentPtr> inits;
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); },
                        [&inits](InitSegmentPtr init) { inits.push_back(init); });

    segmenter.on_video(video_message(0, avc_sequence_header(320, 240)));
    segmenter.on_audio(audio_message(0, aac_sequence_header()));
    feed_video(segmenter, 5, 10, 0);

    const auto epoch_before_change = inits.back()->epoch;

    // A genuine parameter change (different resolution -> different SPS).
    segmenter.on_video(video_message(500, avc_sequence_header(640, 480)));
    EXPECT_EQ(inits.back()->epoch, epoch_before_change + 1);
    ASSERT_FALSE(segments.empty());
    EXPECT_EQ(segments.back()->init_epoch, epoch_before_change); // closed BEFORE the change
}

TEST(DashSegmenterTest, RepeatingTheIdenticalSequenceHeaderDoesNotCutOrRebuild) {
    std::vector<SegmentPtr> segments;
    std::vector<InitSegmentPtr> inits;
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); },
                        [&inits](InitSegmentPtr init) { inits.push_back(init); });

    const auto header = avc_sequence_header();
    segmenter.on_video(video_message(0, header));
    segmenter.on_audio(audio_message(0, aac_sequence_header()));
    feed_video(segmenter, 3, 10, 0);
    const auto init_count_before = inits.size();
    segmenter.on_video(video_message(300, header)); // identical bytes
    EXPECT_EQ(inits.size(), init_count_before);
    EXPECT_TRUE(segments.empty()) << "no cut should have happened";
}

TEST(DashSegmenterTest, SamplesConcatenateBackIntoAWellFormedFragment) {
    std::vector<SegmentPtr> segments;
    SegmenterConfig config;
    config.target_duration = std::chrono::milliseconds(1000);
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); }, [](InitSegmentPtr) {},
                        config);
    segmenter.on_video(video_message(0, avc_sequence_header()));
    segmenter.on_audio(audio_message(0, aac_sequence_header()));
    for (int i = 0; i <= 20; ++i) {
        const std::uint32_t ts = static_cast<std::uint32_t>(i) * 100;
        segmenter.on_video(video_message(ts, avc_frame(i % 10 == 0)));
        segmenter.on_audio(audio_message(ts, aac_frame()));
    }
    segmenter.finalize();

    ASSERT_FALSE(segments.empty());
    for (const auto& segment : segments) {
        const auto view = segment->data.view();
        // styp + moof + mdat: a well-formed fragment tiles the buffer with no
        // gap and no overrun -- reuses the same walk the fMP4 muxer unit
        // tests already validate against, just inline here since this test's
        // point is that the SEGMENTER assembles a valid fragment, not that
        // Fmp4Muxer itself is correct (that's covered elsewhere).
        std::size_t offset = 0;
        bool saw_mdat = false;
        while (offset + 8 <= view.size()) {
            const std::uint32_t size = read_u32(view, offset);
            ASSERT_GE(size, 8u);
            ASSERT_LE(offset + size, view.size());
            if (fourcc(view, offset + 4) == "mdat") saw_mdat = true;
            offset += size;
        }
        EXPECT_EQ(offset, view.size());
        EXPECT_TRUE(saw_mdat);
    }
}

TEST(DashSegmenterTest, PublisherReconnectResetsConfigsAndStartsAFreshTimeline) {
    std::vector<SegmentPtr> segments;
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); }, [](InitSegmentPtr) {});
    segmenter.on_video(video_message(0, avc_sequence_header()));
    segmenter.on_audio(audio_message(0, aac_sequence_header()));
    feed_video(segmenter, 5, 10);

    segmenter.mark_publisher_reconnect();
    EXPECT_FALSE(segmenter.has_video_config());
    EXPECT_FALSE(segmenter.has_audio_config());

    // Media before the new sequence header is dropped, same as a cold start.
    segmenter.on_video(video_message(0, avc_frame(true)));
    EXPECT_GT(segmenter.stats().dropped_frames, 0u);
}
