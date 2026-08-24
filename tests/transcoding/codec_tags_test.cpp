#include <gtest/gtest.h>

#include "rtmp_server/transcoding/native/codec_tags.hpp"

namespace {

using rtmp_server::transcoding::native::h264_level_idc;
using rtmp_server::transcoding::native::hls_codecs_attribute;

TEST(CodecTagsTest, PicksTheLowestLevelThatCarriesTheGeometry) {
    EXPECT_EQ(h264_level_idc(640, 360, 30), 30U);
    // 854x480 is 1620 macroblocks, exactly level 3.0's frame-size limit, but
    // 30fps of them is 48600 MB/s against that level's 40500 ceiling -- the
    // rate limit is what decides here, not the frame size.
    EXPECT_EQ(h264_level_idc(854, 480, 30), 31U);
    EXPECT_EQ(h264_level_idc(854, 480, 25), 30U);
    EXPECT_EQ(h264_level_idc(1280, 720, 30), 31U);
    EXPECT_EQ(h264_level_idc(1280, 720, 60), 32U);
    EXPECT_EQ(h264_level_idc(1920, 1080, 30), 40U);
    EXPECT_EQ(h264_level_idc(1920, 1080, 60), 42U);
    EXPECT_EQ(h264_level_idc(3840, 2160, 30), 51U);
}

// The whole point: a 1080p rung used to be advertised as level 3.1, which is
// a 720p-class limit. Over-declaring is harmless, under-declaring is not.
TEST(CodecTagsTest, NeverUnderDeclaresA1080pRungAs720pClass) {
    EXPECT_GT(h264_level_idc(1920, 1080, 30), h264_level_idc(1280, 720, 30));
}

// Output geometry is unresolved until the source's first frame decodes, and
// the master playlist is published before that.
TEST(CodecTagsTest, DeclaresPermissivelyWhenTheRenditionKeepsTheSourceSize) {
    EXPECT_EQ(h264_level_idc(0, 0, 30), 51U);
    EXPECT_EQ(h264_level_idc(1280, 0, 30), 51U);
}

TEST(CodecTagsTest, RendersHighProfileWithTheLevelAsTwoHexDigits) {
    EXPECT_EQ(hls_codecs_attribute(1280, 720, 30, 128'000), "avc1.64001F,mp4a.40.2");
    EXPECT_EQ(hls_codecs_attribute(1920, 1080, 30, 128'000), "avc1.640028,mp4a.40.2");
}

// build_aac_param_set switches to HE-AACv2 below 64 kbit for a stereo source;
// the playlist has to say so rather than claiming AAC-LC.
TEST(CodecTagsTest, FollowsTheEncodersOwnAacObjectTypeChoice) {
    EXPECT_EQ(hls_codecs_attribute(640, 360, 30, 48'000), "avc1.64001E,mp4a.40.29");
    EXPECT_EQ(hls_codecs_attribute(640, 360, 30, 64'000), "avc1.64001E,mp4a.40.2");
}

TEST(CodecTagsTest, TreatsAZeroFrameRateAsOneRatherThanDividingByIt) {
    EXPECT_EQ(h264_level_idc(1920, 1080, 0), 40U);
}

} // namespace
