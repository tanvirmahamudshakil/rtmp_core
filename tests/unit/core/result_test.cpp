#include <gtest/gtest.h>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::core {
namespace {

TEST(Result, HoldsValueOnSuccess) {
    Result<int> r = 42;
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 42);
}

TEST(Result, HoldsErrorOnFailure) {
    Result<int> r = Error(ErrorCode::Unknown, ErrorCategory::Internal, "boom");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ErrorCode::Unknown);
    EXPECT_EQ(r.error().message(), "boom");
}

TEST(ResultVoid, DefaultConstructedIsOk) {
    Result<void> r;
    EXPECT_TRUE(r.ok());
}

TEST(ResultVoid, ErrorConstructedIsNotOk) {
    Result<void> r = Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration);
    EXPECT_FALSE(r.ok());
}

} // namespace
} // namespace rtmp_server::core
