#include <gtest/gtest.h>

#include <algorithm>

#include "rtmp_server/transcoding/native/geometry.hpp"
#include "rtmp_server/transcoding/native/hevc_params.hpp"
#include "rtmp_server/transcoding/preset.hpp"
#ifdef RTMP_NATIVE_TRANSCODE
#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/hevc_encoder.hpp"
#include "rtmp_server/transcoding/native/scaler.hpp"
#endif

namespace {

using namespace rtmp_server::transcoding;
using namespace rtmp_server::transcoding::native;

Preset make_preset(FitMode fit, std::optional<std::uint32_t> w, std::optional<std::uint32_t> h) {
    Preset preset;
    preset.video_codec = VideoCodec::H265;
    preset.fit_mode = fit;
    preset.width = w;
    preset.height = h;
    return preset;
}

bool even(std::uint32_t v) { return v % 2 == 0; }

TEST(NativeGeometry, MatchSourceKeepsSourceDimensions) {
    const auto plan = compute_scale_plan(make_preset(FitMode::MatchSource, 1280, 720), 1920, 1080);
    EXPECT_EQ(plan.out_w, 1920U);
    EXPECT_EQ(plan.out_h, 1080U);
    EXPECT_TRUE(plan.is_passthrough_size(1920, 1080));
}

TEST(NativeGeometry, FitWidthPreservesAspectRatio) {
    const auto plan = compute_scale_plan(make_preset(FitMode::FitWidth, 1280, std::nullopt), 1920, 1080);
    EXPECT_EQ(plan.out_w, 1280U);
    EXPECT_EQ(plan.out_h, 720U); // 1280 * 1080 / 1920
    EXPECT_TRUE(even(plan.out_w) && even(plan.out_h));
}

TEST(NativeGeometry, FitHeightPreservesAspectRatio) {
    const auto plan = compute_scale_plan(make_preset(FitMode::FitHeight, std::nullopt, 480), 1920, 1080);
    EXPECT_EQ(plan.out_h, 480U);
    EXPECT_EQ(plan.out_w, 852U); // 480 * 1920 / 1080 = 853.33 -> even floor 852
    EXPECT_TRUE(even(plan.out_w));
}

TEST(NativeGeometry, StretchForcesBothDimensions) {
    const auto plan = compute_scale_plan(make_preset(FitMode::Stretch, 640, 640), 1920, 1080);
    EXPECT_EQ(plan.out_w, 640U);
    EXPECT_EQ(plan.out_h, 640U);
    EXPECT_EQ(plan.scale_w, 640U);
    EXPECT_EQ(plan.scale_h, 640U);
}

TEST(NativeGeometry, LetterboxPadsToTargetCanvas) {
    // 16:9 source into a 1:1 target letterboxes top/bottom.
    const auto plan = compute_scale_plan(make_preset(FitMode::Letterbox, 720, 720), 1920, 1080);
    EXPECT_EQ(plan.out_w, 720U);
    EXPECT_EQ(plan.out_h, 720U);
    EXPECT_LE(plan.scale_h, 720U);
    EXPECT_EQ(plan.scale_w, 720U);       // width fills
    EXPECT_GT(plan.pad_y, 0U);           // vertical bars
    EXPECT_EQ(plan.pad_x, 0U);
    EXPECT_TRUE(even(plan.pad_y));
}

TEST(NativeGeometry, CropFillsTargetAndCentres) {
    // 16:9 source into a 1:1 target crops the sides after aspect-fill.
    const auto plan = compute_scale_plan(make_preset(FitMode::Crop, 720, 720), 1920, 1080);
    EXPECT_EQ(plan.out_w, 720U);
    EXPECT_EQ(plan.out_h, 720U);
    EXPECT_GE(plan.scale_w, 720U);
    EXPECT_GE(plan.scale_h, 720U);
    EXPECT_TRUE(even(plan.crop_x) && even(plan.crop_y));
}

// --- x265 parameter mapping ---

std::string value_of(const HevcParamSet& set, const std::string& key) {
    for (const auto& [k, v] : set.options)
        if (k == key) return v;
    return {};
}
bool has_key(const HevcParamSet& set, const std::string& key) {
    return std::any_of(set.options.begin(), set.options.end(),
                       [&](const auto& kv) { return kv.first == key; });
}

TEST(NativeHevcParams, CrfWithVbvIsDefaultRateControl) {
    Preset preset = make_preset(FitMode::MatchSource, std::nullopt, std::nullopt);
    preset.video_bitrate = 2'500'000;
    HevcQualityOptions quality;
    quality.crf = 22.0;

    const auto set = build_hevc_param_set(preset, quality, 1280, 720, 30, 1);
    EXPECT_EQ(value_of(set, "crf"), "22");
    EXPECT_EQ(value_of(set, "vbv-maxrate"), "2500"); // kbit
    EXPECT_EQ(value_of(set, "vbv-bufsize"), "5000");
    EXPECT_FALSE(has_key(set, "bitrate")); // CRF mode, not strict ABR
}

TEST(NativeHevcParams, StrictAbrWhenBitrateConstrained) {
    Preset preset = make_preset(FitMode::MatchSource, std::nullopt, std::nullopt);
    preset.video_bitrate = 900'000;
    HevcQualityOptions quality;
    quality.constrain_to_bitrate = false;

    const auto set = build_hevc_param_set(preset, quality, 854, 480, 30, 1);
    EXPECT_EQ(value_of(set, "bitrate"), "900");
    EXPECT_FALSE(has_key(set, "crf"));
}

TEST(NativeHevcParams, QualityToolsAndFixedGopArePresent) {
    Preset preset = make_preset(FitMode::MatchSource, std::nullopt, std::nullopt);
    preset.keyframe_interval = 48;
    const auto set = build_hevc_param_set(preset, HevcQualityOptions{}, 1280, 720, 30, 1);

    EXPECT_EQ(value_of(set, "aq-mode"), "3");
    EXPECT_EQ(value_of(set, "psy-rd"), "2");
    EXPECT_EQ(value_of(set, "keyint"), "48");
    EXPECT_EQ(value_of(set, "min-keyint"), "48"); // fixed GOP for segment alignment
    EXPECT_EQ(value_of(set, "scenecut"), "0");
    EXPECT_EQ(value_of(set, "annexb"), "1");
    EXPECT_EQ(set.keyint, 48U);
}

TEST(NativeHevcParams, DefaultGopIsTwoSecondsOfFrames) {
    Preset preset = make_preset(FitMode::MatchSource, std::nullopt, std::nullopt);
    const auto set = build_hevc_param_set(preset, HevcQualityOptions{}, 1280, 720, 60, 1);
    EXPECT_EQ(set.keyint, 120U); // 60 fps * 2 s
}

#ifdef RTMP_NATIVE_TRANSCODE
// End-to-end scale + encode, compiled only when the native codec libraries are
// available. Exercises libyuv and x265 against synthetic frames.

YuvFrame make_gradient_frame(std::uint32_t w, std::uint32_t h, std::int64_t pts, int seed) {
    YuvFrame f;
    f.allocate(w, h);
    f.pts_90k = pts;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            f.y[static_cast<std::size_t>(y) * f.y_stride + x] =
                static_cast<std::uint8_t>((x + y + seed) & 0xFF);
    // Chroma left at neutral 0 from allocate(); good enough for a codec smoke test.
    return f;
}

bool starts_with_annexb(const std::vector<std::byte>& b) {
    return b.size() >= 4 && b[0] == std::byte{0} && b[1] == std::byte{0} &&
           ((b[2] == std::byte{1}) || (b[2] == std::byte{0} && b[3] == std::byte{1}));
}

TEST(NativeScaler, DownscalesToPlanGeometry) {
    Scaler scaler;
    const YuvFrame src = make_gradient_frame(1280, 720, 0, 0);
    const auto plan = compute_scale_plan(make_preset(FitMode::FitWidth, 640, std::nullopt), 1280, 720);
    YuvFrame dst;
    ASSERT_TRUE(scaler.scale(src, plan, dst).ok());
    EXPECT_EQ(dst.width, 640U);
    EXPECT_EQ(dst.height, 360U);
    EXPECT_EQ(dst.y.size(), static_cast<std::size_t>(640) * 360);
}

TEST(NativeHevcEncoder, EncodesFramesAndEmitsKeyframe) {
    Preset preset = make_preset(FitMode::MatchSource, std::nullopt, std::nullopt);
    preset.video_bitrate = 1'500'000;
    preset.keyframe_interval = 15;
    HevcQualityOptions quality;
    quality.preset = "ultrafast"; // fast smoke test
    const auto params = build_hevc_param_set(preset, quality, 320, 240, 30, 1);

    HevcEncoder encoder;
    ASSERT_TRUE(encoder.open(params).ok());

    std::vector<EncodedAccessUnit> out;
    for (int i = 0; i < 30; ++i) {
        const YuvFrame f = make_gradient_frame(320, 240, i * 3000, i);
        ASSERT_TRUE(encoder.encode(f, out).ok());
    }
    ASSERT_TRUE(encoder.flush(out).ok());

    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(std::any_of(out.begin(), out.end(), [](const auto& au) { return au.keyframe; }));
    for (const auto& au : out) {
        EXPECT_FALSE(au.annexb.empty());
        EXPECT_TRUE(starts_with_annexb(au.annexb));
    }
}
#endif // RTMP_NATIVE_TRANSCODE

} // namespace
