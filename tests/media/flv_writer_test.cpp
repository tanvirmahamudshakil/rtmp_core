#include "rtmp_server/media/flv/flv_writer.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace rtmp_server::media::flv {
namespace {

std::vector<std::byte> bytes(std::initializer_list<int> vals) {
    std::vector<std::byte> out;
    out.reserve(vals.size());
    for (int v : vals) out.push_back(static_cast<std::byte>(v));
    return out;
}

TEST(FlvWriterTest, FileHeaderIsByteExact) {
    auto h = encode_file_header(/*has_audio=*/true, /*has_video=*/true);
    auto expected = bytes({'F', 'L', 'V', 0x01, 0x05, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00});
    std::vector<std::byte> got(h.begin(), h.end());
    EXPECT_EQ(got, expected);
}

TEST(FlvWriterTest, FileHeaderFlagsReflectTracks) {
    EXPECT_EQ(static_cast<std::uint8_t>(encode_file_header(true, false)[4]), 0x04);
    EXPECT_EQ(static_cast<std::uint8_t>(encode_file_header(false, true)[4]), 0x01);
    EXPECT_EQ(static_cast<std::uint8_t>(encode_file_header(false, false)[4]), 0x00);
}

TEST(FlvWriterTest, AppendTagIsByteExact) {
    std::vector<std::byte> out;
    append_tag(out, kTagTypeVideo, bytes({0xAA, 0xBB}), /*timestamp=*/0);
    auto expected = bytes({
        0x09,                   // TagType = video
        0x00, 0x00, 0x02,       // DataSize = 2
        0x00, 0x00, 0x00,       // Timestamp lower 24 bits
        0x00,                   // TimestampExtended
        0x00, 0x00, 0x00,       // StreamID
        0xAA, 0xBB,             // data
        0x00, 0x00, 0x00, 0x0D, // PreviousTagSize = 11 + 2 = 13
    });
    EXPECT_EQ(out, expected);
}

TEST(FlvWriterTest, AppendTagCarriesExtendedTimestamp) {
    std::vector<std::byte> out;
    // 0x01020304: lower 24 bits = 0x020304, extended byte = 0x01.
    append_tag(out, kTagTypeAudio, bytes({0x00}), 0x01020304u);
    EXPECT_EQ(static_cast<std::uint8_t>(out[4]), 0x02);
    EXPECT_EQ(static_cast<std::uint8_t>(out[5]), 0x03);
    EXPECT_EQ(static_cast<std::uint8_t>(out[6]), 0x04);
    EXPECT_EQ(static_cast<std::uint8_t>(out[7]), 0x01); // extended
}

TEST(FlvWriterTest, DoubleEncodingIsBigEndianIeee754) {
    // 1.0 == 0x3FF0000000000000.
    auto be = encode_double_be(1.0);
    EXPECT_EQ(static_cast<std::uint8_t>(be[0]), 0x3F);
    EXPECT_EQ(static_cast<std::uint8_t>(be[1]), 0xF0);
    for (int i = 2; i < 8; ++i) EXPECT_EQ(static_cast<std::uint8_t>(be[i]), 0x00);
}

TEST(FlvWriterTest, OnMetaDataPlaceholderOffsetsPointAtTheirDoubles) {
    OnMetaData meta;
    meta.duration = 7.0;
    meta.filesize = 123.0;
    auto tag = build_onmetadata_tag(meta);

    // The 8 bytes at each recorded offset must equal that field's encoding.
    auto dur = encode_double_be(7.0);
    auto fsz = encode_double_be(123.0);
    for (std::size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(tag.bytes.at(tag.duration_value_offset + i), dur[i]);
        EXPECT_EQ(tag.bytes.at(tag.filesize_value_offset + i), fsz[i]);
    }
}

TEST(FlvWriterTest, OnMetaDataTagIsAParseableScriptTag) {
    OnMetaData meta;
    std::vector<std::byte> file;
    auto header = encode_file_header(true, true);
    file.insert(file.end(), header.begin(), header.end());
    auto tag = build_onmetadata_tag(meta);
    file.insert(file.end(), tag.bytes.begin(), tag.bytes.end());

    auto parsed = parse_flv(std::span<const std::byte>(file.data(), file.size()));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().tags.size(), 1u);
    EXPECT_EQ(parsed.value().tags[0].type, kTagTypeScriptData);
    EXPECT_EQ(parsed.value().previous_tag_size0, 0u);
}

TEST(FlvWriterTest, ParseRejectsBadSignature) {
    auto data = bytes({'X', 'L', 'V', 0x01, 0x05, 0, 0, 0, 9, 0, 0, 0, 0});
    EXPECT_FALSE(parse_flv(std::span<const std::byte>(data.data(), data.size())).ok());
}

TEST(FlvWriterTest, ParseRejectsTruncatedTag) {
    std::vector<std::byte> file;
    auto header = encode_file_header(true, true);
    file.insert(file.end(), header.begin(), header.end());
    // A tag header claiming DataSize=100 but no data follows.
    for (int b : {0x09, 0x00, 0x00, 0x64, 0, 0, 0, 0, 0, 0, 0}) file.push_back(static_cast<std::byte>(b));
    EXPECT_FALSE(parse_flv(std::span<const std::byte>(file.data(), file.size())).ok());
}


// Phase 8: found by the FLV fuzz harness under ASan (container-overflow in
// read_u32_be via parse_flv). DataOffset was validated only against
// data.size(), but PreviousTagSize0 is read as four bytes starting AT that
// offset — so any DataOffset in [size-3, size] passed validation and then read
// past the end of the buffer.
TEST(FlvWriterTest, ParseRejectsDataOffsetLeavingNoRoomForPreviousTagSize) {
    for (std::uint32_t slack = 0; slack < 4; ++slack) {
        std::vector<std::byte> file;
        auto header = encode_file_header(true, true);
        file.assign(header.begin(), header.end());
        // Pad so the file is comfortably larger than the header, then point
        // DataOffset at a position with fewer than 4 bytes left after it.
        for (int i = 0; i < 32; ++i) file.push_back(std::byte{0});

        const auto offset = static_cast<std::uint32_t>(file.size() - slack);
        file[5] = static_cast<std::byte>((offset >> 24) & 0xFF);
        file[6] = static_cast<std::byte>((offset >> 16) & 0xFF);
        file[7] = static_cast<std::byte>((offset >> 8) & 0xFF);
        file[8] = static_cast<std::byte>(offset & 0xFF);

        EXPECT_FALSE(parse_flv(std::span<const std::byte>(file.data(), file.size())).ok())
            << "DataOffset leaving " << slack << " trailing byte(s) must be rejected";
    }
}

TEST(FlvWriterTest, ParseRejectsDataOffsetPastEndOfFile) {
    std::vector<std::byte> file;
    auto header = encode_file_header(true, true);
    file.assign(header.begin(), header.end());
    for (int i = 0; i < 16; ++i) file.push_back(std::byte{0});

    file[5] = std::byte{0xFF};
    file[6] = std::byte{0xFF};
    file[7] = std::byte{0xFF};
    file[8] = std::byte{0xFF};
    EXPECT_FALSE(parse_flv(std::span<const std::byte>(file.data(), file.size())).ok());
}

} // namespace
} // namespace rtmp_server::media::flv