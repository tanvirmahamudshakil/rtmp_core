#include "rtmp_server/observability/logger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rtmp_server::observability {
namespace {

// Captures every emitted line so the test can assert on real output rather
// than on the logger's internal state.
class CaptureSink {
public:
    CaptureSink() {
        Logger::instance().set_level(LogLevel::Trace);
        Logger::instance().set_sink([this](LogLevel level, std::string_view line) {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.emplace_back(line);
            levels_.push_back(level);
        });
    }

    ~CaptureSink() {
        Logger::instance().reset_sink();
        Logger::instance().set_level(LogLevel::Info);
    }

    [[nodiscard]] std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    [[nodiscard]] std::string all() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string joined;
        for (const auto& line : lines_) joined += line;
        return joined;
    }

    [[nodiscard]] std::size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    std::vector<LogLevel> levels_;
};

// ===========================================================================
// Structured field set
// ===========================================================================

TEST(LoggerStructuredTest, EveryPhase7ContextFieldIsEmittedWhenSet) {
    CaptureSink sink;

    LogContext context;
    context.with_worker(3)
        .with_connection(4242)
        .with_stream(77)
        .with_application(9)
        .with_error_code(-104)
        .with_latency(std::chrono::microseconds{1500})
        .with_request_id("req-abc");

    Logger::instance().log(LogLevel::Info, "event_loop", "connection_closed", context,
                           {LogField{"reason", "peer_reset"}});

    ASSERT_EQ(sink.count(), 1u);
    const std::string line = sink.lines().front();

    // The exact field set docs/v2_promot.md PHASE 7 "Logging" requires.
    EXPECT_NE(line.find("\"timestamp\":\""), std::string::npos);
    EXPECT_NE(line.find("\"level\":\"info\""), std::string::npos);
    EXPECT_NE(line.find("\"component\":\"event_loop\""), std::string::npos);
    EXPECT_NE(line.find("\"event\":\"connection_closed\""), std::string::npos);
    EXPECT_NE(line.find("\"worker_id\":\"3\""), std::string::npos);
    EXPECT_NE(line.find("\"connection_id\":\"4242\""), std::string::npos);
    EXPECT_NE(line.find("\"stream_id\":\"77\""), std::string::npos);
    EXPECT_NE(line.find("\"application_id\":\"9\""), std::string::npos);
    EXPECT_NE(line.find("\"error_code\":\"-104\""), std::string::npos);
    EXPECT_NE(line.find("\"latency_us\":\"1500\""), std::string::npos);
    EXPECT_NE(line.find("\"request_id\":\"req-abc\""), std::string::npos);
    EXPECT_NE(line.find("\"reason\":\"peer_reset\""), std::string::npos);

    EXPECT_EQ(line.back(), '\n');
}

TEST(LoggerStructuredTest, UnsetContextFieldsAreOmittedEntirely) {
    CaptureSink sink;

    LogContext context;
    context.with_connection(7); // handshake stage: no stream/application yet
    Logger::instance().log(LogLevel::Warn, "handshake", "timeout", context);

    const std::string line = sink.lines().front();
    EXPECT_NE(line.find("\"connection_id\":\"7\""), std::string::npos);
    // An empty stream_id would be noise, and would break any consumer that
    // types the field as an integer.
    EXPECT_EQ(line.find("stream_id"), std::string::npos);
    EXPECT_EQ(line.find("application_id"), std::string::npos);
    EXPECT_EQ(line.find("latency_us"), std::string::npos);
}

TEST(LoggerStructuredTest, LevelFilteringSuppressesLowerSeverityRecords) {
    CaptureSink sink;
    Logger::instance().set_level(LogLevel::Warn);

    Logger::instance().log(LogLevel::Info, "c", "should_not_appear", {});
    Logger::instance().log(LogLevel::Debug, "c", "also_not", {});
    Logger::instance().log(LogLevel::Error, "c", "should_appear", {});

    ASSERT_EQ(sink.count(), 1u);
    EXPECT_NE(sink.lines().front().find("should_appear"), std::string::npos);
}

// ===========================================================================
// JSON validity
// ===========================================================================

TEST(LoggerJsonTest, ValuesContainingQuotesBackslashesAndControlsAreEscaped) {
    CaptureSink sink;

    // Before Phase 7 the logger interpolated field values raw, so any value
    // containing a quote produced a malformed JSON line and could inject
    // arbitrary synthetic fields into a log pipeline.
    Logger::instance().log(LogLevel::Info, "test", "escapes",
                           {LogField{"payload", "he said \"hi\"\\and\nnewline\ttab"}});

    const std::string line = sink.lines().front();
    EXPECT_NE(line.find("\\\"hi\\\""), std::string::npos);
    EXPECT_NE(line.find("\\\\and"), std::string::npos);
    EXPECT_NE(line.find("\\n"), std::string::npos);
    EXPECT_NE(line.find("\\t"), std::string::npos);

    // A raw newline inside the value would split one record into two lines,
    // which is fatal for a JSON-lines consumer.
    EXPECT_EQ(std::count(line.begin(), line.end(), '\n'), 1);
}

TEST(LoggerJsonTest, FieldInjectionViaAValueIsNeutralised) {
    CaptureSink sink;

    // A hostile (or merely unlucky) value that tries to close the string and
    // append its own field must not succeed.
    Logger::instance().log(LogLevel::Info, "test", "injection",
                           {LogField{"name", "x\",\"level\":\"error"}});

    const std::string line = sink.lines().front();
    // Exactly one real level field; the injected one is escaped into the value.
    EXPECT_EQ(line.find("\"level\":\"error\""), std::string::npos);
    EXPECT_NE(line.find("\"level\":\"info\""), std::string::npos);
}

TEST(LoggerJsonTest, ControlCharactersBecomeUnicodeEscapes) {
    CaptureSink sink;
    std::string value;
    value += '\x01';
    value += '\x1f';
    Logger::instance().log(LogLevel::Info, "test", "controls", {LogField{"v", value}});

    const std::string line = sink.lines().front();
    EXPECT_NE(line.find("\\u0001"), std::string::npos);
    EXPECT_NE(line.find("\\u001f"), std::string::npos);
}

// ===========================================================================
// Secret redaction — the hard security rule
// ===========================================================================

TEST(LoggerRedactionTest, RedactNeverRevealsEnoughOfASecretToReuseIt) {
    // A realistic 24-byte publish key, the shape StreamManager mints.
    const std::string secret = "sk_live_9f2c4b7e1a6d8305c1b2";

    const std::string redacted = redact(secret);

    // The full secret must never appear.
    EXPECT_EQ(redacted.find(secret), std::string::npos);
    // Nor may any long suffix of it.
    EXPECT_EQ(redacted.find(secret.substr(4)), std::string::npos);
    EXPECT_EQ(redacted.find(secret.substr(8)), std::string::npos);
    // A short correlating prefix is retained deliberately.
    EXPECT_NE(redacted.find("sk_l"), std::string::npos);
    EXPECT_NE(redacted.find("len=28"), std::string::npos);

    // Short values keep nothing at all: a 4-char prefix of a 6-char secret
    // would be a meaningless reduction in guessability.
    EXPECT_EQ(redact("abc123").find("abc"), std::string::npos);
    EXPECT_EQ(redact_fully(secret), "[redacted]");
}

TEST(LoggerRedactionTest, AKnownSecretNeverAppearsInActualLogOutput) {
    CaptureSink sink;

    // The four things PHASE 7 forbids logging.
    const std::string publish_secret = "sk_live_9f2c4b7e1a6d8305c1b2";
    const std::string bearer_token = "eyJhbGciOiJIUzI1NiJ9.QUJDREVGRw.s3cr3tS1gnatur3";
    const std::string password = "hunter2-correct-horse-battery";
    const std::string signed_url = "/v1/streams/live/cam1/playback?token=abcdef0123456789&expires=1700000000";

    // Exactly how a call site is supposed to log these.
    LogContext context;
    context.with_connection(11).with_stream(3);

    Logger::instance().log(LogLevel::Warn, "authenticator", "publish_rejected", context,
                           {LogField{"stream_key", redact(publish_secret)}});
    Logger::instance().log(LogLevel::Warn, "management_api", "unauthorized", context,
                           {LogField{"bearer", redact_fully(bearer_token)}});
    Logger::instance().log(LogLevel::Error, "persistence", "connect_failed", context,
                           {LogField{"credential", redact_fully(password)}});
    Logger::instance().log(LogLevel::Info, "management_api", "request", context,
                           {LogField{"uri", redact_query(signed_url)}});

    const std::string output = sink.all();
    ASSERT_FALSE(output.empty());

    // The hard rule: none of these strings may appear anywhere in the output.
    EXPECT_EQ(output.find(publish_secret), std::string::npos) << "publish secret leaked into logs";
    EXPECT_EQ(output.find(bearer_token), std::string::npos) << "bearer token leaked into logs";
    EXPECT_EQ(output.find(password), std::string::npos) << "credential leaked into logs";
    EXPECT_EQ(output.find("abcdef0123456789"), std::string::npos) << "signed query token leaked into logs";

    // ...while the line is still diagnostically useful.
    EXPECT_NE(output.find("publish_rejected"), std::string::npos);
    EXPECT_NE(output.find("\"connection_id\":\"11\""), std::string::npos);
    EXPECT_NE(output.find("/v1/streams/live/cam1/playback"), std::string::npos);
    EXPECT_NE(output.find("token=[redacted]"), std::string::npos);
}

TEST(LoggerRedactionTest, RedactQueryKeepsParameterNamesButNoValues) {
    EXPECT_EQ(redact_query("/path"), "/path"); // nothing to redact

    const std::string out = redact_query("/play?token=SECRETVALUE&expires=123&debug");
    EXPECT_NE(out.find("/play?"), std::string::npos);
    EXPECT_EQ(out.find("SECRETVALUE"), std::string::npos);
    EXPECT_NE(out.find("token=[redacted]"), std::string::npos);
    // Values are redacted regardless of the parameter name: `expires` is not
    // secret today, but allowlisting by name is how leaks happen tomorrow.
    EXPECT_NE(out.find("expires=[redacted]"), std::string::npos);
    // A bare flag carries no credential and is kept.
    EXPECT_NE(out.find("debug"), std::string::npos);
}

// ===========================================================================
// Concurrency
// ===========================================================================

TEST(LoggerConcurrencyTest, ConcurrentLoggingProducesWholeUntornLines) {
    CaptureSink sink;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t]() {
            LogContext context;
            context.with_worker(static_cast<std::uint32_t>(t));
            for (int i = 0; i < kPerThread; ++i) {
                Logger::instance().log(LogLevel::Info, "concurrency", "tick", context,
                                       {LogField{"i", std::to_string(i)}});
            }
        });
    }
    for (auto& thread : threads) thread.join();

    const auto lines = sink.lines();
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(kThreads * kPerThread));
    for (const auto& line : lines) {
        // Each record is exactly one complete, self-delimited JSON object.
        EXPECT_EQ(line.front(), '{');
        EXPECT_EQ(line.back(), '\n');
        EXPECT_EQ(std::count(line.begin(), line.end(), '\n'), 1);
        EXPECT_NE(line.find("\"event\":\"tick\""), std::string::npos);
    }
}

} // namespace
} // namespace rtmp_server::observability
