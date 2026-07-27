// Phase 8 security task 10: directory-traversal review of the recording path.
//
// These are adversarial tests, not happy-path coverage: every case below is a
// concrete payload an attacker could put in an RTMP `publish` application or
// stream name, and each must be rejected rather than sanitised-into-something
// or written outside the configured recording directory.

#include <gtest/gtest.h>

#include <string>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/recording/recording_path.hpp"

namespace {

using rtmp_server::core::ErrorCode;
using rtmp_server::recording::build_recording_path;
using rtmp_server::recording::is_safe_path_component;
using rtmp_server::recording::kMaxComponentLength;
using rtmp_server::recording::sanitize_path_component;

constexpr std::int64_t kStartedAt = 1'700'000'000;

TEST(RecordingPathTest, BuildsTheExpectedPathForSafeNames) {
    auto path = build_recording_path("/var/lib/rtmp/recordings", "live", "morning-show_01", kStartedAt);
    ASSERT_TRUE(path.ok());
    EXPECT_EQ(path.value(), "/var/lib/rtmp/recordings/live/morning-show_01-1700000000.flv");
}

TEST(RecordingPathTest, AppendsASeparatorWhenTheDirectoryLacksOne) {
    auto with = build_recording_path("/recordings/", "live", "s", kStartedAt);
    auto without = build_recording_path("/recordings", "live", "s", kStartedAt);
    ASSERT_TRUE(with.ok());
    ASSERT_TRUE(without.ok());
    EXPECT_EQ(with.value(), without.value());
}

// --- Directory traversal ---------------------------------------------------

TEST(RecordingPathTest, RejectsClassicDotDotTraversalInTheStreamName) {
    auto path = build_recording_path("/var/lib/rtmp/recordings", "live", "../../../etc/passwd", kStartedAt);
    ASSERT_FALSE(path.ok());
    EXPECT_EQ(path.error().code(), ErrorCode::InvalidArgument);
}

TEST(RecordingPathTest, RejectsTraversalInTheApplicationName) {
    auto path = build_recording_path("/var/lib/rtmp/recordings", "../../etc", "passwd", kStartedAt);
    ASSERT_FALSE(path.ok());
    EXPECT_EQ(path.error().code(), ErrorCode::InvalidArgument);
}

TEST(RecordingPathTest, RejectsBareDirectoryAliases) {
    EXPECT_FALSE(build_recording_path("/rec", "live", "..", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "live", ".", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "..", "s", kStartedAt).ok());
}

TEST(RecordingPathTest, RejectsAbsolutePathInjection) {
    // Would otherwise produce ".../live//etc/cron.d/evil-...flv", and on a
    // path joined with a naive filesystem::path operator/ would discard the
    // configured directory entirely.
    EXPECT_FALSE(build_recording_path("/rec", "live", "/etc/cron.d/evil", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "/etc", "passwd", kStartedAt).ok());
}

TEST(RecordingPathTest, RejectsEmbeddedForwardAndBackSlashes) {
    EXPECT_FALSE(build_recording_path("/rec", "live", "a/b", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "live", "a\\b", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "live", "..\\..\\windows\\system32", kStartedAt).ok());
}

TEST(RecordingPathTest, RejectsNulByteTruncationAttempts) {
    // "evil\0.flv" would truncate at the NUL in any C API taking a char*,
    // yielding a file named "evil" — a classic extension-bypass primitive.
    const std::string name("evil\0ignored", 12);
    EXPECT_FALSE(build_recording_path("/rec", "live", name, kStartedAt).ok());
}

TEST(RecordingPathTest, RejectsPercentEncodedAndOverlongTraversalEncodings) {
    // The allow-list makes these unrepresentable rather than relying on the
    // decoder ordering that makes such bypasses work elsewhere.
    EXPECT_FALSE(build_recording_path("/rec", "live", "..%2f..%2fetc%2fpasswd", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "live", "..%c0%afetc", kStartedAt).ok());
}

TEST(RecordingPathTest, RejectsShellAndGlobMetacharacters) {
    // Not a traversal issue, but these names end up in operator shell
    // one-liners and retention globs; keeping them out of filenames is free.
    for (const auto* name : {"a;rm -rf /", "a$(id)", "a`id`", "a*b", "a|b", "a>b"}) {
        EXPECT_FALSE(build_recording_path("/rec", "live", name, kStartedAt).ok()) << name;
    }
}

TEST(RecordingPathTest, RejectsLeadingDashAndDot) {
    // Leading '-' would be parsed as an option by operator tooling run over
    // the recording directory; leading '.' hides the file from glob sweeps.
    EXPECT_FALSE(build_recording_path("/rec", "live", "-rf", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "live", "--force", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "live", ".hidden", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "-rf", "s", kStartedAt).ok());
    // ...but they remain legal inside the component.
    EXPECT_TRUE(build_recording_path("/rec", "live", "my-stream.v2", kStartedAt).ok());
}

TEST(RecordingPathTest, SanitizeNeutralisesALeadingDashOrDot) {
    EXPECT_EQ(sanitize_path_component("-rf").value(), "_rf");
    EXPECT_EQ(sanitize_path_component(".hidden").value(), "_hidden");
}

TEST(RecordingPathTest, RejectsOverlongComponents) {
    const std::string too_long(kMaxComponentLength + 1, 'a');
    EXPECT_FALSE(build_recording_path("/rec", "live", too_long, kStartedAt).ok());

    const std::string just_right(kMaxComponentLength, 'a');
    EXPECT_TRUE(build_recording_path("/rec", "live", just_right, kStartedAt).ok());
}

TEST(RecordingPathTest, RejectsEmptyComponentsAndDirectory) {
    EXPECT_FALSE(build_recording_path("/rec", "", "s", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("/rec", "live", "", kStartedAt).ok());
    EXPECT_FALSE(build_recording_path("", "live", "s", kStartedAt).ok());
}

TEST(RecordingPathTest, ResultingPathNeverEscapesTheConfiguredDirectory) {
    // Property-style check over the whole adversarial corpus: whatever comes
    // back must start with the configured prefix and contain no "..".
    const std::string root = "/var/lib/rtmp/recordings";
    for (const auto* name : {"../../../etc/passwd", "..", ".", "/etc/passwd", "a/../..", "a\\b", "normal-name"}) {
        auto path = build_recording_path(root, "live", name, kStartedAt);
        if (!path.ok()) continue;
        EXPECT_TRUE(path.value().starts_with(root + "/")) << name;
        EXPECT_EQ(path.value().find(".."), std::string::npos) << name;
    }
}

// --- sanitize_path_component ----------------------------------------------

TEST(RecordingPathTest, SanitizeReplacesUnsafeBytesRatherThanDroppingThem) {
    auto out = sanitize_path_component("a/b\\c:d");
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), "a_b_c_d");
}

TEST(RecordingPathTest, SanitizeStillRejectsDirectoryAliases) {
    // '.' is in the allow-list, so ".." survives byte-level rewriting; the
    // alias check must run afterwards.
    EXPECT_FALSE(sanitize_path_component("..").ok());
    EXPECT_FALSE(sanitize_path_component(".").ok());
    EXPECT_FALSE(sanitize_path_component("").ok());
}

TEST(RecordingPathTest, SanitizeTruncatesToTheComponentLimit) {
    auto out = sanitize_path_component(std::string(kMaxComponentLength * 3, 'x'));
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().size(), kMaxComponentLength);
}

TEST(RecordingPathTest, SanitizedOutputIsAlwaysAcceptedByTheValidator) {
    for (const auto* name : {"a/b", "../../etc", "stream name", "\xff\xfe", "café"}) {
        auto out = sanitize_path_component(name);
        if (!out.ok()) continue;
        EXPECT_TRUE(is_safe_path_component(out.value())) << name;
    }
}

} // namespace
