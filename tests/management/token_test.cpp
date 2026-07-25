#include "rtmp_server/management/token.hpp"

#include <gtest/gtest.h>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::management {
namespace {

TEST(TokenTest, ValidTokenBeforeExpiryVerifiesSuccessfully) {
    auto token = sign_token("secret", "live", "my-show", 2000000000);
    auto result = verify_token("secret", "live", "my-show", token, 2000000000, /*now_unix=*/1000000000);
    EXPECT_TRUE(result.ok());
}

TEST(TokenTest, ExpiredTokenIsRejectedWithExpiredTokenCode) {
    auto token = sign_token("secret", "live", "my-show", 1000);
    auto result = verify_token("secret", "live", "my-show", token, 1000, /*now_unix=*/2000);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::ExpiredToken);
}

TEST(TokenTest, TokenAtExactExpiryInstantIsStillValid) {
    auto token = sign_token("secret", "live", "my-show", 1000);
    auto result = verify_token("secret", "live", "my-show", token, 1000, /*now_unix=*/1000);
    EXPECT_TRUE(result.ok());
}

TEST(TokenTest, ForgedTokenIsRejectedWithUnauthorizedCode) {
    auto result = verify_token("secret", "live", "my-show", "not-a-real-token", 2000000000, 1000000000);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::Unauthorized);
}

TEST(TokenTest, TokenSignedForADifferentStreamNameDoesNotVerify) {
    auto token = sign_token("secret", "live", "show-a", 2000000000);
    auto result = verify_token("secret", "live", "show-b", token, 2000000000, 1000000000);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::Unauthorized);
}

TEST(TokenTest, TokenSignedWithADifferentSecretDoesNotVerify) {
    auto token = sign_token("secret-a", "live", "my-show", 2000000000);
    auto result = verify_token("secret-b", "live", "my-show", token, 2000000000, 1000000000);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::Unauthorized);
}

TEST(TokenTest, TamperedExpiryInvalidatesSignature) {
    // Simulates an attacker editing ?expires= in the URL to extend a token's
    // lifetime without re-signing — must fail signature check, not just
    // silently accept the new expiry.
    auto token = sign_token("secret", "live", "my-show", 1000);
    auto result = verify_token("secret", "live", "my-show", token, /*tampered expires=*/9999999999, 2000000000);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::Unauthorized);
}

} // namespace
} // namespace rtmp_server::management
