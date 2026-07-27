// Phase 8 security tasks 5/6: bounds on the playback-token query parser.

#include <gtest/gtest.h>

#include <string>

#include "rtmp_server/management/query_parser.hpp"

namespace {

using rtmp_server::management::kMaxFieldValueLength;
using rtmp_server::management::kMaxQueryLength;
using rtmp_server::management::kMaxQueryPairs;
using rtmp_server::management::parse_playback_query;
using rtmp_server::management::split_stream_name;

TEST(QueryParserTest, ExtractsTokenAndExpires) {
    auto q = parse_playback_query("token=deadbeef&expires=1700000000");
    ASSERT_TRUE(q.token.has_value());
    EXPECT_EQ(*q.token, "deadbeef");
    ASSERT_TRUE(q.expires_at_unix.has_value());
    EXPECT_EQ(*q.expires_at_unix, 1700000000);
    EXPECT_FALSE(q.truncated);
}

TEST(QueryParserTest, IgnoresUnknownParametersWithoutFailing) {
    auto q = parse_playback_query("utm_source=x&token=abc&cdn=edge1&expires=42&ref=y");
    ASSERT_TRUE(q.token.has_value());
    EXPECT_EQ(*q.token, "abc");
    EXPECT_EQ(q.expires_at_unix.value_or(-1), 42);
}

TEST(QueryParserTest, HandlesEmptyAndDegenerateInput) {
    for (const auto* input : {"", "&", "&&&", "=", "token", "token=", "=abc", "?", "a=b=c"}) {
        auto q = parse_playback_query(input);
        EXPECT_FALSE(q.truncated) << input;
    }
    EXPECT_FALSE(parse_playback_query("token=").token.has_value()); // empty value is not a token
}

TEST(QueryParserTest, FirstOccurrenceWinsSoAppendedDuplicatesCannotOverride) {
    auto q = parse_playback_query("token=signed&expires=100&token=forged&expires=999");
    EXPECT_EQ(q.token.value_or(""), "signed");
    EXPECT_EQ(q.expires_at_unix.value_or(-1), 100);
}

TEST(QueryParserTest, RejectsPartiallyNumericExpiryRatherThanAcceptingItsPrefix) {
    // "123abc" must not silently become 123.
    EXPECT_FALSE(parse_playback_query("expires=123abc").expires_at_unix.has_value());
    EXPECT_FALSE(parse_playback_query("expires=abc").expires_at_unix.has_value());
    EXPECT_FALSE(parse_playback_query("expires= 123").expires_at_unix.has_value());
    EXPECT_FALSE(parse_playback_query("expires=99999999999999999999999").expires_at_unix.has_value());
}

TEST(QueryParserTest, AcceptsNegativeExpiryAsAParsedValue) {
    // Parsing and policy are separate concerns: a negative expiry parses, and
    // then fails the "now <= expires" check downstream.
    EXPECT_EQ(parse_playback_query("expires=-1").expires_at_unix.value_or(0), -1);
}

// --- Bounds ----------------------------------------------------------------

TEST(QueryParserTest, FlagsOverLongQueriesAsTruncated) {
    const std::string query = "token=abc&" + std::string(kMaxQueryLength, 'x') + "=1";
    auto q = parse_playback_query(query);
    EXPECT_TRUE(q.truncated);
}

TEST(QueryParserTest, FlagsOverDenseQueriesAsTruncated) {
    std::string query;
    for (std::size_t i = 0; i < kMaxQueryPairs + 5; ++i) query += "a=b&";
    auto q = parse_playback_query(query);
    EXPECT_TRUE(q.truncated);
}

TEST(QueryParserTest, RejectsOverLongFieldValues) {
    const std::string long_token = "token=" + std::string(kMaxFieldValueLength + 1, 'a');
    EXPECT_FALSE(parse_playback_query(long_token).token.has_value());

    const std::string ok_token = "token=" + std::string(kMaxFieldValueLength, 'a');
    EXPECT_TRUE(parse_playback_query(ok_token).token.has_value());
}

TEST(QueryParserTest, StaysBoundedOnAPathologicallyLargeQuery) {
    // Pre-Phase-8 this built one map entry per pair from an input bounded only
    // by the 10 MiB RTMP message limit. Now the work is capped by
    // kMaxQueryLength/kMaxQueryPairs regardless of input size.
    std::string huge;
    huge.reserve(2u * 1024u * 1024u);
    while (huge.size() < 2u * 1024u * 1024u) huge += "k=v&";
    auto q = parse_playback_query(huge);
    EXPECT_TRUE(q.truncated);
    EXPECT_FALSE(q.token.has_value());
}

TEST(QueryParserTest, SurvivesBinaryAndControlBytes) {
    const std::string binary("token=\x01\x02\xff\x00" "abc&expires=\x7f", 24);
    auto q = parse_playback_query(binary); // must not crash or throw
    EXPECT_FALSE(q.truncated);
}

// --- split_stream_name -----------------------------------------------------

TEST(QueryParserTest, SplitsStreamNameFromQuery) {
    auto s = split_stream_name("mystream?token=abc&expires=1");
    EXPECT_EQ(s.name, "mystream");
    EXPECT_EQ(s.query, "token=abc&expires=1");
}

TEST(QueryParserTest, SplitReturnsWholeInputWhenThereIsNoQuery) {
    auto s = split_stream_name("mystream");
    EXPECT_EQ(s.name, "mystream");
    EXPECT_TRUE(s.query.empty());
}

TEST(QueryParserTest, SplitHandlesEdgeCasePositionsOfTheQuestionMark) {
    EXPECT_TRUE(split_stream_name("?token=a").name.empty());
    EXPECT_TRUE(split_stream_name("name?").query.empty());
    // Only the first '?' splits; later ones belong to the query.
    EXPECT_EQ(split_stream_name("n?a=1?b=2").query, "a=1?b=2");
}

} // namespace
