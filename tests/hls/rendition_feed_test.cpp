#include <gtest/gtest.h>

#include <vector>

#include "rtmp_server/hls/rendition_feed.hpp"
#include "rtmp_server/hls/segmenter.hpp"
#include "rtmp_server/media/ts/ts_muxer.hpp"
#include "test_media.hpp"

using namespace rtmp_server;
using namespace rtmp_server::hls;
using rtmp_server::hls_test::pps_nal;
using rtmp_server::hls_test::sps_nal;

namespace {

// Builds an Annex B access unit from a list of NALs (4-byte start codes).
std::vector<std::byte> annexb(std::initializer_list<std::vector<std::byte>> nals) {
    std::vector<std::byte> out;
    for (const auto& nal : nals) {
        out.push_back(std::byte{0x00});
        out.push_back(std::byte{0x00});
        out.push_back(std::byte{0x00});
        out.push_back(std::byte{0x01});
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

std::vector<std::byte> slice_nal(std::uint8_t nal_type, std::size_t payload) {
    std::vector<std::byte> nal;
    nal.push_back(std::byte{static_cast<std::uint8_t>(0x60 | (nal_type & 0x1F))});
    for (std::size_t i = 0; i < payload; ++i) nal.push_back(std::byte{0x99});
    return nal;
}

// A 7-byte ADTS header (AAC-LC, 44100, stereo, protection_absent) + payload.
std::vector<std::byte> adts_frame(std::size_t payload) {
    const std::size_t frame_len = 7 + payload;
    std::vector<std::byte> f = {
        std::byte{0xFF}, std::byte{0xF1}, std::byte{0x50},
        std::byte{static_cast<std::uint8_t>(0x80 | ((frame_len >> 11) & 0x03))},
        std::byte{static_cast<std::uint8_t>((frame_len >> 3) & 0xFF)},
        std::byte{static_cast<std::uint8_t>(((frame_len & 0x07) << 5) | 0x1F)}, std::byte{0xFC}};
    for (std::size_t i = 0; i < payload; ++i) f.push_back(std::byte{0x55});
    return f;
}

bool is_ts(std::span<const std::byte> d) {
    return !d.empty() && d.size() % media::ts::kPacketSize == 0 &&
           d[0] == std::byte{media::ts::kSyncByte};
}

} // namespace

TEST(RenditionFeed, ReframesEncodedEsIntoHlsSegments) {
    std::vector<SegmentPtr> segments;
    SegmenterConfig config;
    config.target_duration = std::chrono::milliseconds(200); // cut quickly for the test
    Segmenter segmenter([&](SegmentPtr segment) { segments.push_back(std::move(segment)); }, config);
    RenditionFeed feed(segmenter);

    // 3 seconds at 30 fps, a keyframe (with SPS/PPS) every 15 frames.
    const std::uint64_t clock = media::ts::kClockHz;
    for (int i = 0; i < 90; ++i) {
        const bool keyframe = (i % 15) == 0;
        const std::int64_t pts = static_cast<std::int64_t>(i) * clock / 30;
        std::vector<std::byte> au;
        if (keyframe) {
            au = annexb({sps_nal(), pps_nal(), slice_nal(5 /*IDR*/, 200)});
        } else {
            au = annexb({slice_nal(1 /*non-IDR*/, 120)});
        }
        feed.push_video(au, pts, pts, keyframe);
        feed.push_audio(adts_frame(40), pts);
    }
    segmenter.finalize();

    ASSERT_FALSE(segments.empty());
    EXPECT_TRUE(segmenter.has_video_config());
    EXPECT_TRUE(segmenter.has_audio_config());
    for (const auto& segment : segments) {
        ASSERT_NE(segment, nullptr);
        EXPECT_TRUE(is_ts(segment->data.view()));
    }
    // Encoded config should have been derived, so a codecs attribute exists.
    EXPECT_FALSE(segmenter.codecs_attribute().empty());
}
