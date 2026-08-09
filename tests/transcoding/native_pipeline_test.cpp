#include <gtest/gtest.h>

#include <algorithm>

#include "rtmp_server/transcoding/native/aac_params.hpp"
#include "rtmp_server/transcoding/native/geometry.hpp"
#include "rtmp_server/transcoding/native/hevc_params.hpp"
#include "rtmp_server/transcoding/preset.hpp"
#ifdef RTMP_NATIVE_TRANSCODE
#include <cmath>

#include "rtmp_server/transcoding/native/aac_decoder.hpp"
#include "rtmp_server/transcoding/native/aac_encoder.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/h264_encoder.hpp"
#include "rtmp_server/transcoding/native/hevc_encoder.hpp"
#include "rtmp_server/transcoding/native/scaler.hpp"
#include "rtmp_server/transcoding/native/source_transcoder.hpp"
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

// --- AAC audio parameter mapping (pure) ---

TEST(NativeAacParams, FollowsSourceRateAndTargetBitrate) {
    Preset preset;
    preset.audio_bitrate = 128'000;
    const auto set = build_aac_param_set(preset, AacQualityOptions{}, 48000, 2);
    EXPECT_EQ(set.sample_rate, 48000U); // no resampling
    EXPECT_EQ(set.channels, 2U);
    EXPECT_EQ(set.bitrate, 128'000U);
    EXPECT_EQ(set.profile, AacProfile::LowComplexity);
    EXPECT_EQ(set.audio_object_type(), 2);
}

TEST(NativeAacParams, AutoSelectsHeAacV2ForLowBitrateStereo) {
    Preset preset;
    preset.audio_bitrate = 48'000; // below the 64k auto-HE threshold
    const auto set = build_aac_param_set(preset, AacQualityOptions{}, 44100, 2);
    EXPECT_EQ(set.profile, AacProfile::HighEfficiencyV2);
    EXPECT_EQ(set.audio_object_type(), 29);
}

TEST(NativeAacParams, AutoHeFallsBackToHeAacForMono) {
    Preset preset;
    preset.audio_bitrate = 48'000;
    const auto set = build_aac_param_set(preset, AacQualityOptions{}, 44100, 1);
    EXPECT_EQ(set.profile, AacProfile::HighEfficiency); // PS needs stereo
    EXPECT_EQ(set.channels, 1U);
}

TEST(NativeAacParams, RespectsExplicitProfileWhenAutoDisabled) {
    Preset preset;
    preset.audio_bitrate = 32'000;
    AacQualityOptions quality;
    quality.allow_auto_profile = false;
    const auto set = build_aac_param_set(preset, quality, 44100, 2);
    EXPECT_EQ(set.profile, AacProfile::LowComplexity);
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

TEST(NativeH264Encoder, EncodesFramesRealtimeAndEmitsKeyframe) {
    const auto config = build_h264_config(320, 240, 30, 1'000'000, 15, 1);
    H264Encoder encoder;
    ASSERT_TRUE(encoder.open(config).ok());

    std::vector<EncodedAccessUnit> out;
    for (int i = 0; i < 30; ++i) {
        const YuvFrame f = make_gradient_frame(320, 240, i * 3000, i);
        ASSERT_TRUE(encoder.encode(f, out).ok());
    }
    ASSERT_FALSE(out.empty());
    EXPECT_TRUE(std::any_of(out.begin(), out.end(), [](const auto& au) { return au.keyframe; }));
    for (const auto& au : out) {
        EXPECT_FALSE(au.annexb.empty());
        EXPECT_TRUE(starts_with_annexb(au.annexb));
        EXPECT_EQ(au.dts_90k, au.pts_90k); // no B-frames
    }
}

TEST(NativeH264Encoder, MultiSliceOutputRemainsDecodable) {
    auto config = build_h264_config(640, 360, 30, 1'000'000, 15, 2);
    config.allow_frame_skip = false;
    H264Encoder encoder;
    ASSERT_TRUE(encoder.open(config).ok());

    H264Decoder decoder;
    ASSERT_TRUE(decoder.initialize().ok());
    YuvFrame decoded;
    bool decoded_picture = false;
    std::vector<EncodedAccessUnit> out;
    for (int i = 0; i < 12; ++i) {
        const YuvFrame frame = make_gradient_frame(640, 360, i * 3000, i);
        ASSERT_TRUE(encoder.encode(frame, out).ok());
        for (const auto& access_unit : out) {
            bool produced = false;
            ASSERT_TRUE(
                decoder.decode(access_unit.annexb, access_unit.pts_90k, decoded, produced).ok());
            decoded_picture = decoded_picture || produced;
        }
        out.clear();
    }
    EXPECT_TRUE(decoded_picture);
    EXPECT_EQ(decoded.width, 640U);
    EXPECT_EQ(decoded.height, 360U);
}

PcmBlock make_sine_block(std::uint32_t sample_rate, std::uint32_t channels, std::uint32_t frames,
                         double phase_start) {
    PcmBlock block;
    block.sample_rate = sample_rate;
    block.channels = channels;
    block.samples.resize(static_cast<std::size_t>(frames) * channels);
    const double step = 2.0 * 3.14159265358979 * 440.0 / sample_rate;
    for (std::uint32_t f = 0; f < frames; ++f) {
        const auto value =
            static_cast<std::int16_t>(std::sin(phase_start + f * step) * 12000.0);
        for (std::uint32_t c = 0; c < channels; ++c)
            block.samples[static_cast<std::size_t>(f) * channels + c] = value;
    }
    return block;
}

bool is_adts(const std::vector<std::byte>& b) {
    // ADTS syncword is 12 set bits: 0xFF followed by 0xF0 in the top nibble.
    return b.size() >= 7 && b[0] == std::byte{0xFF} && (std::to_integer<int>(b[1]) & 0xF0) == 0xF0;
}

TEST(NativeAacEncoder, EncodesPcmToAdtsFrames) {
    AacParamSet params;
    params.profile = AacProfile::LowComplexity;
    params.sample_rate = 44100;
    params.channels = 2;
    params.bitrate = 128'000;

    AacEncoder encoder;
    ASSERT_TRUE(encoder.open(params).ok());
    EXPECT_EQ(encoder.frame_length(), 1024U);

    std::vector<EncodedAudioFrame> out;
    double phase = 0.0;
    for (int i = 0; i < 20; ++i) {
        const PcmBlock block = make_sine_block(44100, 2, 1024, phase);
        phase += 1024 * 2.0 * 3.14159265358979 * 440.0 / 44100;
        ASSERT_TRUE(encoder.encode(block, out).ok());
    }
    ASSERT_TRUE(encoder.flush(out).ok());

    ASSERT_FALSE(out.empty());
    for (const auto& frame : out) {
        EXPECT_TRUE(is_adts(frame.adts));
        EXPECT_EQ(frame.samples_per_channel, 1024U);
    }
}

TEST(NativeAacDecoder, DrainsEveryAdtsFrameFromOnePes) {
    AacParamSet params;
    params.profile = AacProfile::LowComplexity;
    params.sample_rate = 48000;
    params.channels = 2;
    params.bitrate = 128'000;

    AacEncoder encoder;
    ASSERT_TRUE(encoder.open(params).ok());
    std::vector<EncodedAudioFrame> encoded;
    double phase = 0.0;
    for (int i = 0; i < 8; ++i) {
        const PcmBlock block = make_sine_block(48000, 2, 1024, phase);
        phase += 1024 * 2.0 * 3.14159265358979 * 440.0 / 48000;
        ASSERT_TRUE(encoder.encode(block, encoded).ok());
    }
    ASSERT_TRUE(encoder.flush(encoded).ok());
    ASSERT_GE(encoded.size(), 3U);

    std::vector<std::byte> pes;
    std::size_t expected_frames = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        pes.insert(pes.end(), encoded[i].adts.begin(), encoded[i].adts.end());
        expected_frames += encoded[i].samples_per_channel;
    }

    AacDecoder decoder;
    ASSERT_TRUE(decoder.configure_adts().ok());
    PcmBlock decoded;
    bool produced = false;
    ASSERT_TRUE(decoder.decode(pes, decoded, produced).ok());
    ASSERT_TRUE(produced);
    EXPECT_EQ(decoded.sample_rate, 48000U);
    EXPECT_EQ(decoded.channels, 2U);
    EXPECT_EQ(decoded.frames(), expected_frames);
}
TEST(NativeSourceTranscoder, FansOneSourceOutToMultipleRenditions) {
    // Build a decodable H.264 source by encoding gradient frames (keyframe
    // carries SPS/PPS), then transcode that source into two renditions.
    const auto source_config = build_h264_config(320, 240, 30, 1'500'000, 15, 1);
    H264Encoder source_encoder;
    ASSERT_TRUE(source_encoder.open(source_config).ok());
    std::vector<EncodedAccessUnit> source_aus;
    for (int i = 0; i < 20; ++i) {
        const YuvFrame f = make_gradient_frame(320, 240, i * 3000, i);
        ASSERT_TRUE(source_encoder.encode(f, source_aus).ok());
    }
    ASSERT_FALSE(source_aus.empty());

    std::vector<RenditionSpec> renditions = {
        {"480p-ish", "out_480", 240, 240, 800'000, 15, 96'000, FitMode::Letterbox},
        {"240p", "out_240", 160, 120, 400'000, 15, 64'000},
    };
    SourceTranscoder transcoder(renditions, 30);
    ASSERT_TRUE(transcoder.start().ok());
    ASSERT_EQ(transcoder.rendition_count(), 2U);

    std::vector<int> per_rendition_outputs(2, 0);
    bool saw_keyframe = false;
    bool saw_exact_letterbox_size = false;
    H264Decoder rendition_decoder;
    ASSERT_TRUE(rendition_decoder.initialize().ok());
    YuvFrame rendition_frame;
    transcoder.set_video_output([&](std::size_t rendition, const EncodedAccessUnit& au) {
        ASSERT_LT(rendition, per_rendition_outputs.size());
        EXPECT_FALSE(au.annexb.empty());
        EXPECT_TRUE(starts_with_annexb(au.annexb));
        per_rendition_outputs[rendition]++;
        if (au.keyframe) saw_keyframe = true;
        if (rendition == 0) {
            bool produced = false;
            ASSERT_TRUE(rendition_decoder.decode(au.annexb, au.pts_90k, rendition_frame, produced).ok());
            if (produced) {
                EXPECT_EQ(rendition_frame.width, 240U);
                EXPECT_EQ(rendition_frame.height, 240U);
                saw_exact_letterbox_size = true;
            }
        }
    });

    for (const auto& au : source_aus) {
        ASSERT_TRUE(transcoder.on_video(au.annexb, au.pts_90k, au.dts_90k, au.keyframe).ok());
    }

    EXPECT_GT(per_rendition_outputs[0], 0);
    EXPECT_GT(per_rendition_outputs[1], 0);
    EXPECT_TRUE(saw_keyframe);
    EXPECT_TRUE(saw_exact_letterbox_size);
}

TEST(NativeSourceTranscoder, ReducesSixtyFpsInputToConfiguredThirtyFpsWithMonotonicTimestamps) {
    auto source_config = build_h264_config(320, 240, 60, 2'000'000, 60, 1);
    source_config.allow_frame_skip = false;
    H264Encoder source_encoder;
    ASSERT_TRUE(source_encoder.open(source_config).ok());

    std::vector<EncodedAccessUnit> source_aus;
    for (int i = 0; i < 60; ++i) {
        // Reproduce millisecond-rounded 60 fps timestamps seen in raw MPEG-TS
        // sources: adjacent deltas vary around 1500 ticks and two deltas can
        // sum to 2970 rather than exactly 3000.
        const auto pts_90k = static_cast<std::int64_t>(i * 1000 / 60) * 90;
        const YuvFrame frame = make_gradient_frame(320, 240, pts_90k, i);
        ASSERT_TRUE(source_encoder.encode(frame, source_aus).ok());
    }
    ASSERT_GE(source_aus.size(), 58U);

    std::vector<RenditionSpec> renditions = {
        {"30fps", "out_30", 320, 240, 1'000'000, 30, 96'000},
    };
    SourceTranscoder transcoder(renditions, 30);
    ASSERT_TRUE(transcoder.start().ok());

    std::vector<std::int64_t> output_pts;
    transcoder.set_video_output(
        [&](std::size_t rendition, const EncodedAccessUnit& au) {
            EXPECT_EQ(rendition, 0U);
            output_pts.push_back(au.pts_90k);
            EXPECT_EQ(au.dts_90k, au.pts_90k);
        });

    for (const auto& au : source_aus) {
        ASSERT_TRUE(transcoder.on_video(au.annexb, au.pts_90k, au.dts_90k, au.keyframe).ok());
    }

    EXPECT_GE(output_pts.size(), 28U);
    EXPECT_LE(output_pts.size(), 31U);
    for (std::size_t i = 1; i < output_pts.size(); ++i) {
        EXPECT_GE(output_pts[i] - output_pts[i - 1], 3000);
    }
}

TEST(NativeSourceTranscoder, KeepsMillisecondRoundedThirtyFpsInputAtThirtyFps) {
    auto source_config = build_h264_config(320, 240, 30, 1'500'000, 30, 1);
    source_config.allow_frame_skip = false;
    H264Encoder source_encoder;
    ASSERT_TRUE(source_encoder.open(source_config).ok());

    std::vector<EncodedAccessUnit> source_aus;
    for (int i = 0; i < 60; ++i) {
        // TS demuxers often expose a millisecond clock before conversion to
        // 90kHz: 33ms, 33ms, 34ms. These are genuine 30fps frames even though
        // two out of three deltas are 2970 rather than exactly 3000 ticks.
        const auto pts_90k = static_cast<std::int64_t>(i * 1000 / 30) * 90;
        ASSERT_TRUE(source_encoder
                        .encode(make_gradient_frame(320, 240, pts_90k, i), source_aus)
                        .ok());
    }
    ASSERT_GE(source_aus.size(), 58U);

    SourceTranscoder transcoder({{"30fps", "out_30", 320, 240, 1'000'000, 30, 96'000}}, 30);
    ASSERT_TRUE(transcoder.start().ok());
    std::vector<std::int64_t> output_pts;
    transcoder.set_video_output(
        [&](std::size_t, const EncodedAccessUnit& au) { output_pts.push_back(au.pts_90k); });
    for (const auto& au : source_aus) {
        ASSERT_TRUE(transcoder.on_video(au.annexb, au.pts_90k, au.dts_90k, au.keyframe).ok());
    }

    // OpenH264 can retain the last couple of decoded pictures until a flush;
    // the important regression bound is that normal 2970-tick deltas no
    // longer discard roughly one third of the stream.
    EXPECT_GE(output_pts.size(), 55U);
    EXPECT_LE(output_pts.size(), 60U);
    for (std::size_t i = 1; i < output_pts.size(); ++i) {
        EXPECT_EQ(output_pts[i] - output_pts[i - 1], 3000);
    }
}

TEST(NativeSourceTranscoder, KeepsOutputClockMonotonicAcrossExplicitSourceGap) {
    auto source_config = build_h264_config(320, 240, 30, 1'500'000, 10, 1);
    source_config.allow_frame_skip = false;
    H264Encoder source_encoder;
    ASSERT_TRUE(source_encoder.open(source_config).ok());

    std::vector<EncodedAccessUnit> source_aus;
    for (int i = 0; i < 24; ++i) {
        ASSERT_TRUE(source_encoder
                        .encode(make_gradient_frame(320, 240, i * 3000, i), source_aus)
                        .ok());
    }
    ASSERT_GE(source_aus.size(), 20U);

    SourceTranscoder transcoder({{"30fps", "out_30", 320, 240, 1'000'000, 10, 96'000}}, 30);
    ASSERT_TRUE(transcoder.start().ok());
    std::vector<std::int64_t> output_pts;
    transcoder.set_video_output(
        [&](std::size_t, const EncodedAccessUnit& au) { output_pts.push_back(au.pts_90k); });

    for (const auto& au : source_aus) {
        ASSERT_TRUE(transcoder.on_video(au.annexb, au.pts_90k, au.dts_90k, au.keyframe).ok());
    }
    const auto before_gap = output_pts.size();
    ASSERT_GT(before_gap, 10U);

    // Re-feed a fresh timestamp epoch, as an upstream HLS discontinuity can
    // do. The first output after mark_discontinuity must follow the prior
    // generated PTS; it must not jump back to raw input PTS zero.
    transcoder.mark_discontinuity();
    for (const auto& au : source_aus) {
        ASSERT_TRUE(transcoder.on_video(au.annexb, au.pts_90k, au.dts_90k, au.keyframe).ok());
    }

    ASSERT_GT(output_pts.size(), before_gap);
    for (std::size_t i = 1; i < output_pts.size(); ++i) {
        EXPECT_EQ(output_pts[i] - output_pts[i - 1], 3000);
    }
}

TEST(NativeSourceTranscoder, ReEncodesAudioPerRendition) {
    // Make an ADTS source with the AAC encoder, then re-encode per rendition.
    AacParamSet source_params;
    source_params.sample_rate = 44100;
    source_params.channels = 2;
    source_params.bitrate = 128'000;
    AacEncoder source_encoder;
    ASSERT_TRUE(source_encoder.open(source_params).ok());
    std::vector<EncodedAudioFrame> adts_frames;
    double phase = 0.0;
    for (int i = 0; i < 180; ++i) {
        const PcmBlock block = make_sine_block(44100, 2, 1024, phase);
        phase += 1024 * 2.0 * 3.14159265358979 * 440.0 / 44100;
        ASSERT_TRUE(source_encoder.encode(block, adts_frames).ok());
    }
    ASSERT_FALSE(adts_frames.empty());

    std::vector<RenditionSpec> renditions = {{"hi", "out_hi", 0, 0, 0, 15, 96'000},
                                             {"lo", "out_lo", 0, 0, 0, 15, 48'000}};
    SourceTranscoder transcoder(renditions, 30);
    ASSERT_TRUE(transcoder.start().ok());

    // Establish video's real clock before audio starts. Keep it stationary
    // afterwards: the old 3-second hard re-anchor then jumped audio back to
    // this point, which makes this a deterministic regression test.
    const auto video_config = build_h264_config(320, 240, 30, 1'000'000, 30, 1);
    H264Encoder video_encoder;
    ASSERT_TRUE(video_encoder.open(video_config).ok());
    std::vector<EncodedAccessUnit> video;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(video_encoder.encode(make_gradient_frame(320, 240, i * 3000, i), video).ok());
    }
    ASSERT_FALSE(video.empty());
    for (const auto& frame : video) {
        ASSERT_TRUE(transcoder.on_video(frame.annexb, frame.pts_90k, frame.dts_90k,
                                        frame.keyframe).ok());
    }

    std::vector<int> audio_outputs(2, 0);
    std::vector<std::int64_t> audio_pts;
    transcoder.set_audio_output(
        [&](std::size_t rendition, const EncodedAudioFrame& frame, std::int64_t pts) {
            ASSERT_LT(rendition, audio_outputs.size());
            EXPECT_FALSE(frame.adts.empty());
            EXPECT_GE(pts, 0);
            audio_outputs[rendition]++;
            if (rendition == 0) audio_pts.push_back(pts);
        });

    std::int64_t pts = 0;
    for (const auto& frame : adts_frames) {
        auto r = transcoder.on_audio(frame.adts, pts);
        ASSERT_TRUE(r.ok()) << r.error().message();
        pts += 1024 * 90000 / 44100;
    }

    EXPECT_GT(audio_outputs[0], 0);
    EXPECT_GT(audio_outputs[1], 0);
    ASSERT_GT(audio_pts.size(), 140U);
    for (std::size_t i = 1; i < audio_pts.size(); ++i) {
        EXPECT_GT(audio_pts[i], audio_pts[i - 1]);
    }
}

#endif // RTMP_NATIVE_TRANSCODE

} // namespace
