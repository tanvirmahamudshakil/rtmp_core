#include <gtest/gtest.h>

#include "rtmp_server/core/random.hpp"

namespace rtmp_server::core {
namespace {

TEST(Random, SecureTokenHasExpectedHexLength) {
    auto token = generate_secure_token(32);
    EXPECT_EQ(token.size(), 64u);
    for (char c : token) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

TEST(Random, SecureTokensAreNotTriviallyEqual) {
    EXPECT_NE(generate_secure_token(16), generate_secure_token(16));
}

TEST(Random, UuidV4HasCorrectFormat) {
    auto uuid = generate_uuid_v4();
    ASSERT_EQ(uuid.size(), 36u);
    EXPECT_EQ(uuid[8], '-');
    EXPECT_EQ(uuid[13], '-');
    EXPECT_EQ(uuid[14], '4'); // version nibble
    EXPECT_EQ(uuid[18], '-');
    EXPECT_EQ(uuid[23], '-');
}

TEST(Random, Id64IsNonZero) {
    for (int i = 0; i < 100; ++i) {
        EXPECT_NE(generate_id64(), 0u);
    }
}

} // namespace
} // namespace rtmp_server::core
