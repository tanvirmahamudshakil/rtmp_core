#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <optional>
#include <string>
#include <vector>

#include "rtmp_server/media/mp4/fmp4_muxer.hpp"

using namespace rtmp_server;
using namespace rtmp_server::media;

namespace {

std::uint32_t read_u32(std::span<const std::byte> data, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<std::uint8_t>(data[offset + i]);
    }
    return value;
}

std::uint64_t read_u64(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint64_t>(read_u32(data, offset)) << 32) | read_u32(data, offset + 4);
}

std::string fourcc(std::span<const std::byte> data, std::size_t offset) {
    std::string out;
    for (std::size_t i = 0; i < 4; ++i) out.push_back(static_cast<char>(data[offset + i]));
    return out;
}

// Walks the box tree, checking that every box's declared size lands exactly on
// the next box and that the outermost boxes tile the buffer with no gap. This
// is the property a demuxer depends on and the one a size back-patching bug
// breaks; checking it structurally catches far more than asserting on a few
// known offsets would.
struct BoxWalk {
    bool valid = true;
    std::vector<std::string> types; // depth-first, container children included
};

const std::vector<std::string>& container_types() {
    static const std::vector<std::string> kContainers = {
        "moov", "trak", "mdia", "minf", "stbl", "dinf", "moof", "traf", "mvex"};
    return kContainers;
}

bool is_container(const std::string& type) {
    const auto& list = container_types();
    return std::find(list.begin(), list.end(), type) != list.end();
}

void walk(std::span<const std::byte> data, std::size_t begin, std::size_t end, BoxWalk& out) {
    std::size_t offset = begin;
    while (offset + 8 <= end) {
        const std::uint32_t size = read_u32(data, offset);
        const std::string type = fourcc(data, offset + 4);
        if (size < 8 || offset + size > end) {
            out.valid = false;
            return;
        }
        out.types.push_back(type);
        if (is_container(type)) {
            walk(data, offset + 8, offset + size, out);
        }
        offset += size;
    }
    if (offset != end) out.valid = false;
}

BoxWalk walk_all(std::span<const std::byte> data) {
    BoxWalk out;
    walk(data, 0, data.size(), out);
    return out;
}

// Finds a top-level box by type; returns its offset, or nullopt.
std::optional<std::size_t> find_top_level(std::span<const std::byte> data, const std::string& type) {
    std::size_t offset = 0;
    while (offset + 8 <= data.size()) {
        const std::uint32_t size = read_u32(data, offset);
        if (size < 8 || offset + size > data.size()) return std::nullopt;
        if (fourcc(data, offset + 4) == type) return offset;
        offset += size;
    }
    return std::nullopt;
}

bool contains(const BoxWalk& walk, const std::string& type) {
    return std::find(walk.types.begin(), walk.types.end(), type) != walk.types.end();
}

std::size_t count_of(const BoxWalk& walk, const std::string& type) {
    return static_cast<std::size_t>(std::count(walk.types.begin(), walk.types.end(), type));
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (unsigned v : values) out.push_back(static_cast<std::byte>(v & 0xFF));
    return out;
}

// An AVCDecoderConfigurationRecord for High profile, level 3.1. The muxer
// copies it into `avcC` verbatim, so only its first four bytes matter here.
std::vector<std::byte> avcc_record() {
    return bytes_of({0x01, 0x64, 0x00, 0x1F, 0xFF, 0xE1, 0x00, 0x04, 0x67, 0x64, 0x00, 0x1F, 0x01,
                     0x00, 0x02, 0x68, 0xEE});
}

mp4::Fmp4InitConfig h264_aac_config() {
    mp4::Fmp4InitConfig config;
    config.video_codec = mp4::VideoCodec::H264;
    config.video_decoder_config = avcc_record();
    config.video_dimensions = {1280, 720};
    config.has_audio = true;
    config.audio_config = {2, 4, 2}; // AAC-LC, 44100 Hz, stereo
    config.audio_specific_config = bytes_of({0x12, 0x10});
    return config;
}

} // namespace

TEST(Fmp4MuxerTest, InitSegmentBoxTreeIsWellFormed) {
    mp4::Fmp4Muxer muxer;
    const auto init = muxer.init_segment(h264_aac_config());
    ASSERT_TRUE(init.ok()) << std::string(init.error().message());

    const auto walk = walk_all(init.value());
    ASSERT_TRUE(walk.valid) << "box sizes do not tile the buffer";
    EXPECT_TRUE(contains(walk, "ftyp"));
    EXPECT_TRUE(contains(walk, "moov"));
    EXPECT_TRUE(contains(walk, "mvhd"));
    EXPECT_EQ(count_of(walk, "trak"), 2u);
    EXPECT_TRUE(contains(walk, "mvex"));
    EXPECT_EQ(count_of(walk, "trex"), 2u);
    // Without stsd's sample entry the tracks carry no decoder configuration,
    // and a player has nothing to initialise.
    EXPECT_EQ(count_of(walk, "stsd"), 2u);
}

TEST(Fmp4MuxerTest, VideoOnlyAndAudioOnlyInitSegmentsCarryOneTrack) {
    mp4::Fmp4Muxer muxer;

    auto video_only = h264_aac_config();
    video_only.has_audio = false;
    const auto video_init = muxer.init_segment(video_only);
    ASSERT_TRUE(video_init.ok());
    auto walk = walk_all(video_init.value());
    ASSERT_TRUE(walk.valid);
    EXPECT_EQ(count_of(walk, "trak"), 1u);
    EXPECT_EQ(count_of(walk, "trex"), 1u);

    auto audio_only = h264_aac_config();
    audio_only.video_codec = mp4::VideoCodec::None;
    audio_only.video_decoder_config.clear();
    const auto audio_init = muxer.init_segment(audio_only);
    ASSERT_TRUE(audio_init.ok());
    walk = walk_all(audio_init.value());
    ASSERT_TRUE(walk.valid);
    EXPECT_EQ(count_of(walk, "trak"), 1u);
}

TEST(Fmp4MuxerTest, InitSegmentRejectsAnIncompleteConfiguration) {
    mp4::Fmp4Muxer muxer;

    mp4::Fmp4InitConfig empty;
    EXPECT_FALSE(muxer.init_segment(empty).ok());

    auto no_config = h264_aac_config();
    no_config.video_decoder_config.clear();
    EXPECT_FALSE(muxer.init_segment(no_config).ok());

    auto no_dimensions = h264_aac_config();
    no_dimensions.video_dimensions = {};
    EXPECT_FALSE(muxer.init_segment(no_dimensions).ok());

    auto no_asc = h264_aac_config();
    no_asc.audio_specific_config.clear();
    EXPECT_FALSE(muxer.init_segment(no_asc).ok());
}

TEST(Fmp4MuxerTest, FragmentDataOffsetsPointAtTheRightSampleBytes) {
    mp4::Fmp4Muxer muxer;

    const std::vector<std::byte> video_sample(300, std::byte{0xAA});
    const std::vector<std::byte> audio_sample(100, std::byte{0xBB});

    mp4::Fmp4TrackFragment video;
    video.base_decode_time = 90000;
    video.samples.push_back({video_sample, 3000, 0, true});
    video.samples.push_back({video_sample, 3000, 3000, false});

    mp4::Fmp4TrackFragment audio;
    audio.base_decode_time = 44100;
    audio.samples.push_back({audio_sample, 1024, 0, true});

    std::vector<std::byte> out;
    ASSERT_TRUE(muxer.write_fragment(out, video, audio, 44100).ok());

    const auto walk = walk_all(out);
    ASSERT_TRUE(walk.valid);
    EXPECT_EQ(count_of(walk, "traf"), 2u);
    EXPECT_TRUE(contains(walk, "mdat"));

    const auto moof_offset = find_top_level(out, "moof");
    const auto mdat_offset = find_top_level(out, "mdat");
    ASSERT_TRUE(moof_offset.has_value());
    ASSERT_TRUE(mdat_offset.has_value());
    const std::uint32_t moof_size = read_u32(out, *moof_offset);

    // mdat must hold exactly the sample bytes, video first.
    const std::uint32_t mdat_size = read_u32(out, *mdat_offset);
    EXPECT_EQ(mdat_size, 8u + 300u + 300u + 100u);
    EXPECT_EQ(static_cast<std::uint8_t>(out[*mdat_offset + 8]), 0xAAu);
    EXPECT_EQ(static_cast<std::uint8_t>(out[*mdat_offset + 8 + 600]), 0xBBu);

    // Each trun's data_offset is relative to the start of the moof
    // (tfhd default-base-is-moof). Following it from the moof must land on
    // that track's first sample byte.
    std::vector<std::uint32_t> data_offsets;
    for (std::size_t i = *moof_offset; i + 8 <= out.size(); ++i) {
        if (fourcc(out, i) != "trun") continue;
        // trun: size(4) type(4) version+flags(4) sample_count(4) data_offset(4)
        data_offsets.push_back(read_u32(out, i - 4 + 16));
    }
    ASSERT_EQ(data_offsets.size(), 2u);
    EXPECT_EQ(data_offsets[0], moof_size + 8u);
    EXPECT_EQ(data_offsets[1], moof_size + 8u + 600u);
    EXPECT_EQ(static_cast<std::uint8_t>(out[*moof_offset + data_offsets[0]]), 0xAAu);
    EXPECT_EQ(static_cast<std::uint8_t>(out[*moof_offset + data_offsets[1]]), 0xBBu);
}

TEST(Fmp4MuxerTest, TfdtCarriesTheFragmentsBaseDecodeTime) {
    mp4::Fmp4Muxer muxer;
    const std::vector<std::byte> sample(64, std::byte{0x11});

    mp4::Fmp4TrackFragment video;
    video.base_decode_time = 1234567;
    video.samples.push_back({sample, 3000, 0, true});

    std::vector<std::byte> out;
    ASSERT_TRUE(muxer.write_fragment(out, video, {}, 44100).ok());

    std::optional<std::uint64_t> base_decode_time;
    for (std::size_t i = 0; i + 8 <= out.size(); ++i) {
        if (fourcc(out, i) != "tfdt") continue;
        // tfdt: type at i, then version+flags(4), then a 64-bit time (v1).
        base_decode_time = read_u64(out, i + 8);
        break;
    }
    ASSERT_TRUE(base_decode_time.has_value());
    EXPECT_EQ(*base_decode_time, 1234567u);
}

TEST(Fmp4MuxerTest, FragmentSequenceNumbersIncreaseAndResetOnDemand) {
    mp4::Fmp4Muxer muxer;
    const std::vector<std::byte> sample(16, std::byte{0x22});
    mp4::Fmp4TrackFragment video;
    video.samples.push_back({sample, 3000, 0, true});

    auto sequence_of = [](std::span<const std::byte> data) {
        // mfhd: size(4) 'mfhd' version+flags(4) sequence_number(4)
        for (std::size_t i = 0; i + 16 <= data.size(); ++i) {
            if (fourcc(data, i) == "mfhd") return read_u32(data, i + 8);
        }
        return 0u;
    };

    std::vector<std::byte> first;
    ASSERT_TRUE(muxer.write_fragment(first, video, {}, 44100).ok());
    std::vector<std::byte> second;
    ASSERT_TRUE(muxer.write_fragment(second, video, {}, 44100).ok());
    EXPECT_EQ(sequence_of(first), 1u);
    EXPECT_EQ(sequence_of(second), 2u);

    muxer.reset();
    std::vector<std::byte> third;
    ASSERT_TRUE(muxer.write_fragment(third, video, {}, 44100).ok());
    EXPECT_EQ(sequence_of(third), 1u);
}

TEST(Fmp4MuxerTest, AnEmptyFragmentIsRejected) {
    mp4::Fmp4Muxer muxer;
    std::vector<std::byte> out;
    EXPECT_FALSE(muxer.write_fragment(out, {}, {}, 44100).ok());
    EXPECT_TRUE(out.empty());
}

TEST(Fmp4MuxerTest, StypIsASelfContainedSegmentTypeBox) {
    std::vector<std::byte> out;
    mp4::Fmp4Muxer::write_styp(out);
    const auto walk = walk_all(out);
    ASSERT_TRUE(walk.valid);
    ASSERT_EQ(walk.types.size(), 1u);
    EXPECT_EQ(walk.types[0], "styp");
}

TEST(Fmp4MuxerTest, CodecStringsFollowRfc6381) {
    // avc1.PPCCLL straight out of the configuration record's bytes 1-3.
    EXPECT_EQ(mp4::video_codec_string(mp4::VideoCodec::H264, avcc_record()), "avc1.64001f");
    // Too short to derive anything: an empty string, never a guess.
    EXPECT_EQ(mp4::video_codec_string(mp4::VideoCodec::H264, bytes_of({0x01, 0x64})), "");
    EXPECT_EQ(mp4::video_codec_string(mp4::VideoCodec::None, avcc_record()), "");

    // hvc1: profile_space 0, Main profile (1), compatibility flags with only
    // bit 1 set, Main tier, level 120 (4.0), no constraint bytes.
    const auto hvcc = bytes_of({0x01, 0x01, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x78});
    EXPECT_EQ(mp4::video_codec_string(mp4::VideoCodec::Hevc, hvcc), "hvc1.1.6.L120");

    aac::AudioSpecificConfig aac_lc{2, 4, 2};
    EXPECT_EQ(mp4::audio_codec_string(aac_lc), "mp4a.40.2");
    aac::AudioSpecificConfig he_aac{5, 4, 2};
    EXPECT_EQ(mp4::audio_codec_string(he_aac), "mp4a.40.5");
}
