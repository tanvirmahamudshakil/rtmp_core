#include <gtest/gtest.h>

// HevcDecoder is only built when the native codec libraries (including
// libde265) are available -- see src/transcoding/CMakeLists.txt's
// RTMP_NATIVE_TRANSCODE_AVAILABLE gate. There is no local h264_decoder_test.cpp
// to mirror (H264Decoder's decode-path coverage lives inside
// native_pipeline_test.cpp's NativeH264Decoder/NativeH264Encoder tests, e.g.
// MultiSliceOutputRemainsDecodable's encode-then-decode round trip); this
// file follows that same round-trip shape for HEVC, since no bundled HEVC
// bitstream fixture exists under tests/**/fixtures to decode directly:
// HevcEncoder produces a real, self-contained Annex B HEVC access unit
// (VPS/SPS/PPS + IDR slice) that HevcDecoder then decodes, so the test
// exercises real libde265 decode logic rather than a hand-built bitstream.
#ifdef RTMP_NATIVE_TRANSCODE

#include <algorithm>
#include <cstdint>
#include <vector>

#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/hevc_decoder.hpp"
#include "rtmp_server/transcoding/native/hevc_encoder.hpp"
#include "rtmp_server/transcoding/native/hevc_params.hpp"
#include "rtmp_server/transcoding/preset.hpp"

namespace {

using namespace rtmp_server::transcoding;
using namespace rtmp_server::transcoding::native;

Preset make_hevc_preset() {
    Preset preset;
    preset.video_codec = VideoCodec::H265;
    preset.fit_mode = FitMode::MatchSource;
    preset.video_bitrate = 1'500'000;
    preset.keyframe_interval = 15;
    return preset;
}

YuvFrame make_gradient_frame(std::uint32_t w, std::uint32_t h, std::int64_t pts, int seed) {
    YuvFrame f;
    f.allocate(w, h);
    f.pts_90k = pts;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            f.y[static_cast<std::size_t>(y) * f.y_stride + x] =
                static_cast<std::uint8_t>((x + y + seed) & 0xFF);
    return f;
}

} // namespace

TEST(NativeHevcDecoder, DecodesEncoderOutputToExpectedDimensions) {
    constexpr std::uint32_t kWidth = 320;
    constexpr std::uint32_t kHeight = 240;

    Preset preset = make_hevc_preset();
    HevcQualityOptions quality;
    quality.preset = "ultrafast"; // fast smoke test
    const auto params = build_hevc_param_set(preset, quality, kWidth, kHeight, 30, 1);

    HevcEncoder encoder;
    ASSERT_TRUE(encoder.open(params).ok());

    HevcDecoder decoder;
    ASSERT_TRUE(decoder.initialize().ok());

    YuvFrame decoded;
    bool decoded_picture = false;
    std::vector<EncodedAccessUnit> out;
    // A handful of frames, not just one: the first access unit alone
    // (VPS/SPS/PPS + IDR slice) is enough for libde265 to produce a picture,
    // but exercising a short run guards against the encoder emitting only
    // parameter sets on frame 0 with the IDR slice trailing in a later
    // access unit for some encoder/build configurations.
    for (int i = 0; i < 5; ++i) {
        const YuvFrame frame = make_gradient_frame(kWidth, kHeight, i * 3000, i);
        ASSERT_TRUE(encoder.encode(frame, out).ok());
        for (const auto& access_unit : out) {
            ASSERT_FALSE(access_unit.annexb.empty());
            bool produced = false;
            auto result = decoder.decode(access_unit.annexb, access_unit.pts_90k, decoded, produced);
            ASSERT_TRUE(result.ok()) << (result.ok() ? "" : result.error().message());
            decoded_picture = decoded_picture || produced;
        }
        out.clear();
    }

    ASSERT_TRUE(decoded_picture);
    EXPECT_EQ(decoded.width, kWidth);
    EXPECT_EQ(decoded.height, kHeight);
    // A basic sanity check that real picture data came back, not a
    // zero-filled buffer left over from a decode that silently no-op'd.
    EXPECT_FALSE(decoded.y.empty());
    const bool has_nonzero_luma =
        std::any_of(decoded.y.begin(), decoded.y.end(), [](std::uint8_t v) { return v != 0; });
    EXPECT_TRUE(has_nonzero_luma);
}

#endif // RTMP_NATIVE_TRANSCODE
