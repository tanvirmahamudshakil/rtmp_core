#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"

namespace rtmp_server::protocol::amf0 {
namespace {

std::vector<std::byte> encode_one(const Amf0Value& v) { return encode(v); }

TEST(Amf0CodecTest, NumberRoundTrips) {
    auto bytes = encode_one(Amf0Value::number(3.5));
    ASSERT_EQ(bytes[0], static_cast<std::byte>(Amf0Marker::Number));
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().bytes_consumed, bytes.size());
    EXPECT_TRUE(decoded.value().value.is_number());
    EXPECT_DOUBLE_EQ(decoded.value().value.as_number(), 3.5);
}

TEST(Amf0CodecTest, NegativeAndZeroNumbers) {
    for (double v : {0.0, -0.0, -42.125, 1e300, -1e-300}) {
        auto bytes = encode_one(Amf0Value::number(v));
        auto decoded = decode(bytes);
        ASSERT_TRUE(decoded.ok());
        EXPECT_DOUBLE_EQ(decoded.value().value.as_number(), v);
    }
}

TEST(Amf0CodecTest, BooleanRoundTrips) {
    for (bool v : {true, false}) {
        auto bytes = encode_one(Amf0Value::boolean(v));
        ASSERT_EQ(bytes.size(), 2u);
        auto decoded = decode(bytes);
        ASSERT_TRUE(decoded.ok());
        EXPECT_TRUE(decoded.value().value.is_boolean());
        EXPECT_EQ(decoded.value().value.as_boolean(), v);
    }
}

TEST(Amf0CodecTest, ShortStringRoundTrips) {
    auto bytes = encode_one(Amf0Value::string("connect"));
    EXPECT_EQ(bytes[0], static_cast<std::byte>(Amf0Marker::String));
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().value.as_string(), "connect");
    EXPECT_EQ(decoded.value().bytes_consumed, bytes.size());
}

TEST(Amf0CodecTest, EmptyStringRoundTrips) {
    auto bytes = encode_one(Amf0Value::string(""));
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().value.as_string(), "");
}

TEST(Amf0CodecTest, LongStringUsesLongStringMarkerAndRoundTrips) {
    std::string long_str(70000, 'x');
    auto bytes = encode_one(Amf0Value::string(long_str));
    ASSERT_EQ(bytes[0], static_cast<std::byte>(Amf0Marker::LongString));
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().value.as_string(), long_str);
    EXPECT_EQ(decoded.value().bytes_consumed, bytes.size());
}

TEST(Amf0CodecTest, NullRoundTrips) {
    auto bytes = encode_one(Amf0Value::null());
    EXPECT_EQ(bytes.size(), 1u);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_TRUE(decoded.value().value.is_null());
}

TEST(Amf0CodecTest, UndefinedRoundTrips) {
    auto bytes = encode_one(Amf0Value::undefined());
    EXPECT_EQ(bytes.size(), 1u);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_TRUE(decoded.value().value.is_undefined());
}

TEST(Amf0CodecTest, ObjectRoundTripsWithNestedValues) {
    Amf0Value obj = Amf0Value::object({
        {"app", Amf0Value::string("live")},
        {"flashVer", Amf0Value::string("FMLE/3.0")},
        {"audioSampleRate", Amf0Value::number(44100)},
        {"fpad", Amf0Value::boolean(false)},
    });
    auto bytes = encode_one(obj);
    ASSERT_EQ(bytes[0], static_cast<std::byte>(Amf0Marker::Object));
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    ASSERT_TRUE(decoded.value().value.is_object());
    EXPECT_EQ(decoded.value().bytes_consumed, bytes.size());
    EXPECT_EQ(decoded.value().value.find("app")->as_string(), "live");
    EXPECT_EQ(decoded.value().value.find("audioSampleRate")->as_number(), 44100);
    EXPECT_FALSE(decoded.value().value.find("fpad")->as_boolean());
    EXPECT_EQ(decoded.value().value, obj);
}

TEST(Amf0CodecTest, EmptyObjectRoundTrips) {
    auto bytes = encode_one(Amf0Value::object({}));
    // marker(1) + terminator(u16=0 + ObjectEnd marker = 3) = 4
    EXPECT_EQ(bytes.size(), 4u);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_TRUE(decoded.value().value.is_object());
    EXPECT_TRUE(decoded.value().value.as_object().empty());
}

TEST(Amf0CodecTest, EcmaArrayRoundTrips) {
    Amf0Value arr = Amf0Value::ecma_array({
        {"level", Amf0Value::string("status")},
        {"code", Amf0Value::string("NetStream.Publish.Start")},
    });
    auto bytes = encode_one(arr);
    ASSERT_EQ(bytes[0], static_cast<std::byte>(Amf0Marker::EcmaArray));
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    ASSERT_TRUE(decoded.value().value.is_ecma_array());
    EXPECT_EQ(decoded.value().value.find("code")->as_string(), "NetStream.Publish.Start");
    EXPECT_EQ(decoded.value().bytes_consumed, bytes.size());
}

TEST(Amf0CodecTest, StrictArrayRoundTrips) {
    Amf0Value arr = Amf0Value::strict_array(
        {Amf0Value::number(1), Amf0Value::string("two"), Amf0Value::boolean(true), Amf0Value::null()});
    auto bytes = encode_one(arr);
    ASSERT_EQ(bytes[0], static_cast<std::byte>(Amf0Marker::StrictArray));
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    ASSERT_TRUE(decoded.value().value.is_strict_array());
    const auto& items = decoded.value().value.as_strict_array();
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0].as_number(), 1);
    EXPECT_EQ(items[1].as_string(), "two");
    EXPECT_TRUE(items[2].as_boolean());
    EXPECT_TRUE(items[3].is_null());
}

TEST(Amf0CodecTest, DateRoundTrips) {
    auto bytes = encode_one(Amf0Value::date(1234567890.0, 0));
    ASSERT_EQ(bytes[0], static_cast<std::byte>(Amf0Marker::Date));
    ASSERT_EQ(bytes.size(), 11u); // marker + 8-byte double + 2-byte timezone
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    ASSERT_TRUE(decoded.value().value.is_date());
    EXPECT_DOUBLE_EQ(decoded.value().value.as_date().milliseconds, 1234567890.0);
    EXPECT_EQ(decoded.value().value.as_date().timezone, 0);
}

TEST(Amf0CodecTest, NestedObjectInsideObjectRoundTrips) {
    Amf0Value inner = Amf0Value::object({{"x", Amf0Value::number(1)}});
    Amf0Value outer = Amf0Value::object({{"inner", inner}, {"y", Amf0Value::number(2)}});
    auto bytes = encode_one(outer);
    auto decoded = decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().value, outer);
}

TEST(Amf0CodecTest, DecodeAllParsesConcatenatedCommandPayload) {
    std::vector<std::byte> payload;
    encode(Amf0Value::string("connect"), payload);
    encode(Amf0Value::number(1), payload);
    encode(Amf0Value::object({{"app", Amf0Value::string("live")}}), payload);

    auto decoded = decode_all(payload);
    ASSERT_TRUE(decoded.ok());
    ASSERT_EQ(decoded.value().size(), 3u);
    EXPECT_EQ(decoded.value()[0].as_string(), "connect");
    EXPECT_EQ(decoded.value()[1].as_number(), 1);
    EXPECT_EQ(decoded.value()[2].find("app")->as_string(), "live");
}

TEST(Amf0CodecTest, TruncatedNumberIsRejected) {
    std::vector<std::byte> bytes{static_cast<std::byte>(Amf0Marker::Number), std::byte{0}, std::byte{0}};
    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), core::ErrorCode::MalformedAmf);
}

TEST(Amf0CodecTest, TruncatedStringLengthPrefixIsRejected) {
    std::vector<std::byte> bytes{static_cast<std::byte>(Amf0Marker::String)};
    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), core::ErrorCode::MalformedAmf);
}

TEST(Amf0CodecTest, StringLengthExceedingAvailableBytesIsRejected) {
    std::vector<std::byte> bytes{static_cast<std::byte>(Amf0Marker::String), std::byte{0}, std::byte{10}};
    bytes.push_back(std::byte{'h'}); // only 1 byte of the claimed 10
    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.ok());
}

TEST(Amf0CodecTest, ObjectMissingTerminatorIsRejected) {
    std::vector<std::byte> bytes{static_cast<std::byte>(Amf0Marker::Object)};
    // one property "a": Number(1), then truncate before the terminator
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{1});
    bytes.push_back(std::byte{'a'});
    bytes.push_back(static_cast<std::byte>(Amf0Marker::Number));
    for (int i = 0; i < 8; ++i) bytes.push_back(std::byte{0});
    // missing terminator entirely
    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), core::ErrorCode::MalformedAmf);
}

TEST(Amf0CodecTest, UnknownMarkerIsRejected) {
    std::vector<std::byte> bytes{std::byte{0xFF}};
    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), core::ErrorCode::MalformedAmf);
}

TEST(Amf0CodecTest, UnsupportedMarkerIsRejected) {
    std::vector<std::byte> bytes{static_cast<std::byte>(Amf0Marker::Reference), std::byte{0}, std::byte{0}};
    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.ok());
}

TEST(Amf0CodecTest, EmptyInputIsRejected) {
    std::vector<std::byte> bytes;
    auto decoded = decode(bytes);
    EXPECT_FALSE(decoded.ok());
}

TEST(Amf0CodecTest, DecodeAllRejectsTrailingGarbage) {
    std::vector<std::byte> bytes;
    encode(Amf0Value::number(1), bytes);
    bytes.push_back(static_cast<std::byte>(Amf0Marker::String)); // dangling partial value
    auto decoded = decode_all(bytes);
    EXPECT_FALSE(decoded.ok());
}

} // namespace
} // namespace rtmp_server::protocol::amf0
