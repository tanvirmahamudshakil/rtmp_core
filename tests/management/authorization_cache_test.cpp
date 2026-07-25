#include "rtmp_server/management/authorization_cache.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace rtmp_server::management {
namespace {

TEST(AuthorizationCacheTest, CachesLoaderResultAndDoesNotReinvokeWithinTtl) {
    int calls = 0;
    AuthorizationCache cache(
        [&](std::string_view, std::string_view) {
            ++calls;
            return true;
        },
        std::chrono::milliseconds(60000));

    EXPECT_TRUE(cache.authorize("live", "key-a"));
    EXPECT_TRUE(cache.authorize("live", "key-a"));
    EXPECT_TRUE(cache.authorize("live", "key-a"));
    EXPECT_EQ(calls, 1);
}

TEST(AuthorizationCacheTest, DifferentApplicationOrKeyIsANewCacheEntry) {
    int calls = 0;
    AuthorizationCache cache(
        [&](std::string_view, std::string_view) {
            ++calls;
            return true;
        },
        std::chrono::milliseconds(60000));

    (void)cache.authorize("live", "key-a");
    (void)cache.authorize("vod", "key-a"); // different application
    (void)cache.authorize("live", "key-b"); // different key
    EXPECT_EQ(calls, 3);
}

TEST(AuthorizationCacheTest, CachesNegativeResultsToo) {
    int calls = 0;
    AuthorizationCache cache(
        [&](std::string_view, std::string_view) {
            ++calls;
            return false;
        },
        std::chrono::milliseconds(60000));

    EXPECT_FALSE(cache.authorize("live", "bad-key"));
    EXPECT_FALSE(cache.authorize("live", "bad-key"));
    EXPECT_EQ(calls, 1);
}

TEST(AuthorizationCacheTest, ExpiredEntryReinvokesTheLoader) {
    int calls = 0;
    AuthorizationCache cache(
        [&](std::string_view, std::string_view) {
            ++calls;
            return true;
        },
        std::chrono::milliseconds(1));

    (void)cache.authorize("live", "key-a");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)cache.authorize("live", "key-a");
    EXPECT_EQ(calls, 2);
}

TEST(AuthorizationCacheTest, InvalidateForcesTheNextCallToReinvokeTheLoader) {
    int calls = 0;
    AuthorizationCache cache(
        [&](std::string_view, std::string_view) {
            ++calls;
            return true;
        },
        std::chrono::milliseconds(60000));

    (void)cache.authorize("live", "key-a");
    cache.invalidate("live", "key-a");
    (void)cache.authorize("live", "key-a");
    EXPECT_EQ(calls, 2);
}

} // namespace
} // namespace rtmp_server::management
