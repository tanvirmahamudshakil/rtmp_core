#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rtmp_server/media/ts/ts_demuxer.hpp"
#include "rtmp_server/media/ts/ts_muxer.hpp"

using namespace rtmp_server;
using namespace rtmp_server::media::ts;

namespace {

std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (auto v : values) out.push_back(std::byte{v});
    return out;
}

// A minimal Annex B access unit: one NAL with the given type.
std::vector<std::byte> annexb_nal(std::uint8_t nal_type, std::initializer_list<std::uint8_t> body) {
    std::vector<std::byte> out = bytes_of({0x00, 0x00, 0x00, 0x01});
    out.push_back(std::byte{static_cast<std::uint8_t>(0x60 | (nal_type & 0x1F))});
    for (auto v : body) out.push_back(std::byte{v});
    return out;
}

struct CapturedVideo {
    std::vector<std::byte> annexb;
    std::uint64_t pts = 0;
    std::uint64_t dts = 0;
    bool keyframe = false;
};
struct CapturedAudio {
    std::vector<std::byte> adts;
    std::uint64_t pts = 0;
};

} // namespace

TEST(TsDemuxer, RoundTripsVideoAndAudioThroughMuxer) {
    TsMuxer muxer;
    std::vector<std::byte> ts;
    muxer.write_program_tables(ts);

    const auto key_au = annexb_nal(5 /*IDR*/, {0x11, 0x22, 0x33, 0x44});
    const auto delta_au = annexb_nal(1 /*non-IDR*/, {0x55, 0x66});
    const auto adts_frame = bytes_of({0xFF, 0xF1, 0x50, 0x80, 0x00, 0x1F, 0xFC, 0xAA, 0xBB, 0xCC});

    ASSERT_TRUE(muxer.write_video(ts, key_au, /*pts*/ 9000, /*dts*/ 9000, /*keyframe*/ true).ok());
    ASSERT_TRUE(muxer.write_audio(ts, adts_frame, /*pts*/ 9000).ok());
    muxer.write_program_tables(ts);
    ASSERT_TRUE(muxer.write_video(ts, delta_au, /*pts*/ 12000, /*dts*/ 12000, false).ok());

    std::vector<CapturedVideo> videos;
    std::vector<CapturedAudio> audios;
    TsDemuxer demuxer;
    demuxer.set_video_handler([&](std::span<const std::byte> annexb, std::uint64_t pts,
                                  std::uint64_t dts, bool keyframe) {
        videos.push_back({{annexb.begin(), annexb.end()}, pts, dts, keyframe});
    });
    demuxer.set_audio_handler([&](std::span<const std::byte> adts, std::uint64_t pts) {
        audios.push_back({{adts.begin(), adts.end()}, pts});
    });

    ASSERT_TRUE(demuxer.feed(ts).ok());
    demuxer.flush();

    ASSERT_EQ(videos.size(), 2U);
    EXPECT_EQ(videos[0].annexb, key_au);
    EXPECT_EQ(videos[0].pts, 9000U);
    EXPECT_TRUE(videos[0].keyframe);
    EXPECT_EQ(videos[1].annexb, delta_au);
    EXPECT_EQ(videos[1].pts, 12000U);
    EXPECT_FALSE(videos[1].keyframe);

    ASSERT_EQ(audios.size(), 1U);
    EXPECT_EQ(audios[0].adts, adts_frame);
    EXPECT_EQ(audios[0].pts, 9000U);
}

TEST(TsDemuxer, ResyncsOnGarbagePrefixAndSplitFeeds) {
    TsMuxer muxer;
    std::vector<std::byte> ts;
    muxer.write_program_tables(ts);
    const auto au = annexb_nal(5, {0xDE, 0xAD, 0xBE, 0xEF});
    ASSERT_TRUE(muxer.write_video(ts, au, 4500, 4500, true).ok());

    // Prepend junk the demuxer must skip, then feed in two arbitrary slices to
    // exercise the partial-packet buffering across feed() calls.
    std::vector<std::byte> stream = bytes_of({0x00, 0x13, 0x37});
    stream.insert(stream.end(), ts.begin(), ts.end());

    std::vector<CapturedVideo> videos;
    TsDemuxer demuxer;
    demuxer.set_video_handler([&](std::span<const std::byte> annexb, std::uint64_t pts,
                                  std::uint64_t dts, bool keyframe) {
        videos.push_back({{annexb.begin(), annexb.end()}, pts, dts, keyframe});
    });

    const std::size_t cut = stream.size() / 3;
    ASSERT_TRUE(demuxer.feed(std::span(stream).subspan(0, cut)).ok());
    ASSERT_TRUE(demuxer.feed(std::span(stream).subspan(cut)).ok());
    demuxer.flush();

    ASSERT_EQ(videos.size(), 1U);
    EXPECT_EQ(videos[0].annexb, au);
    EXPECT_TRUE(videos[0].keyframe);
    EXPECT_EQ(videos[0].pts, 4500U);
}
