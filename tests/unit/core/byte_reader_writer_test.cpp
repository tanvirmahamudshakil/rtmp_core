#include <gtest/gtest.h>

#include <array>

#include "rtmp_server/core/byte_reader.hpp"
#include "rtmp_server/core/byte_writer.hpp"

namespace rtmp_server::core {
namespace {

TEST(ByteWriter, WritesBigEndianIntegers) {
    ByteWriter writer;
    writer.write_u16_be(0x0102);
    writer.write_u24_be(0x030405);
    writer.write_u32_be(0x06070809);

    const auto& data = writer.data();
    ASSERT_EQ(data.size(), 9u);
    EXPECT_EQ(static_cast<unsigned>(data[0]), 0x01);
    EXPECT_EQ(static_cast<unsigned>(data[1]), 0x02);
    EXPECT_EQ(static_cast<unsigned>(data[2]), 0x03);
    EXPECT_EQ(static_cast<unsigned>(data[8]), 0x09);
}

TEST(ByteReader, RoundTripsThroughWriter) {
    ByteWriter writer;
    writer.write_u16_be(0xBEEF);
    writer.write_u24_be(0xABCDEF);
    writer.write_u32_be(0xDEADBEEF);

    ByteReader reader(writer.data());
    EXPECT_EQ(reader.read_u16_be(), 0xBEEFu);
    EXPECT_EQ(reader.read_u24_be(), 0xABCDEFu);
    EXPECT_EQ(reader.read_u32_be(), 0xDEADBEEFu);
    EXPECT_EQ(reader.remaining(), 0u);
}

TEST(ByteReader, RejectsReadsPastEnd) {
    std::array<std::byte, 1> data{std::byte{0x01}};
    ByteReader reader(data);

    EXPECT_FALSE(reader.read_u16_be().has_value());
    EXPECT_FALSE(reader.read_u32_be().has_value());
    EXPECT_TRUE(reader.read_u8().has_value());
    EXPECT_FALSE(reader.read_u8().has_value());
}

TEST(ByteReader, ReadBytesRespectsBounds) {
    std::array<std::byte, 4> data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    ByteReader reader(data);

    auto slice = reader.read_bytes(4);
    ASSERT_TRUE(slice.has_value());
    EXPECT_EQ(slice->size(), 4u);
    EXPECT_FALSE(reader.read_bytes(1).has_value());
}

} // namespace
} // namespace rtmp_server::core
