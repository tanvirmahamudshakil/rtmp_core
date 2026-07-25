#include "rtmp_server/core/hmac.hpp"

#include <gtest/gtest.h>

namespace rtmp_server::core {
namespace {

TEST(HmacTest, Sha256HexOfEmptyStringMatchesKnownVector) {
    // NIST/RFC test vector for SHA-256("").
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(HmacTest, Sha256HexOfAbcMatchesKnownVector) {
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HmacTest, HmacSha256IsDeterministic) {
    EXPECT_EQ(hmac_sha256_hex("secret", "message"), hmac_sha256_hex("secret", "message"));
}

TEST(HmacTest, HmacSha256DiffersByKey) {
    EXPECT_NE(hmac_sha256_hex("secret-a", "message"), hmac_sha256_hex("secret-b", "message"));
}

TEST(HmacTest, HmacSha256DiffersByMessage) {
    EXPECT_NE(hmac_sha256_hex("secret", "message-a"), hmac_sha256_hex("secret", "message-b"));
}

TEST(HmacTest, ConstantTimeEqualsMatchesForEqualStrings) {
    EXPECT_TRUE(constant_time_equals("abcdef", "abcdef"));
}

TEST(HmacTest, ConstantTimeEqualsRejectsDifferentContent) {
    EXPECT_FALSE(constant_time_equals("abcdef", "abcxyz"));
}

TEST(HmacTest, ConstantTimeEqualsRejectsDifferentLengths) {
    EXPECT_FALSE(constant_time_equals("abc", "abcdef"));
}

TEST(HmacTest, ConstantTimeEqualsAcceptsTwoEmptyStrings) {
    EXPECT_TRUE(constant_time_equals("", ""));
}

} // namespace
} // namespace rtmp_server::core
