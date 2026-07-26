#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rtmp_server::observability {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error };

// A single structured field, e.g. {"connection_id", "42"}. Values are
// pre-stringified by the caller to keep the logger free of type templates on
// the hot path.
struct LogField {
    std::string_view key;
    std::string value;
};

// ---------------------------------------------------------------------------
// Redaction (docs/v2_promot.md PHASE 7 "Never log: full publish secret, full
// bearer token, sensitive query string, private credentials").
//
// These are the ONLY sanctioned way to get a secret-derived value into a log
// line. They are deliberately lossy and non-invertible-by-eye:
//
//   redact(secret)         -> "abcd…(len=32)"  (<=4 leading chars kept)
//   redact_fully(secret)   -> "[redacted]"
//   redact_query(uri)      -> path kept, every query VALUE replaced
//
// `redact` keeps at most a 4-character prefix so an operator can correlate a
// log line with a key they already hold, without the line ever containing
// enough material to authenticate. Values shorter than 8 characters are
// redacted entirely, because a 4-char prefix of a 6-char secret is not a
// meaningful reduction in guessability.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string redact(std::string_view secret);
[[nodiscard]] std::string redact_fully(std::string_view secret);

// Strips credential-bearing query parameter values from a URI/URL, keeping
// parameter names and the path so the log line is still diagnosable.
[[nodiscard]] std::string redact_query(std::string_view uri);

// ---------------------------------------------------------------------------
// LogContext: the Phase 7 required structured field set. Every member is
// optional; only the ones that are set are emitted, so a log line from the
// handshake path (no stream yet) does not carry an empty stream_id.
//
// Note what is NOT here: no stream key, no token, no password, no remote
// credential. Identity is carried by the numeric StreamId/ApplicationId the
// server itself minted (see protocol/commands/stream_ids.hpp), never by the
// publish secret the client presented.
// ---------------------------------------------------------------------------
struct LogContext {
    std::optional<std::uint32_t> worker_id;
    std::optional<std::uint64_t> connection_id;
    std::optional<std::uint64_t> stream_id;
    std::optional<std::uint64_t> application_id;
    std::optional<std::int32_t> error_code;
    std::optional<std::chrono::microseconds> latency;
    std::optional<std::string> request_id;

    // Fluent setters so a call site reads as one expression.
    LogContext& with_worker(std::uint32_t v) { worker_id = v; return *this; }
    LogContext& with_connection(std::uint64_t v) { connection_id = v; return *this; }
    LogContext& with_stream(std::uint64_t v) { stream_id = v; return *this; }
    LogContext& with_application(std::uint64_t v) { application_id = v; return *this; }
    LogContext& with_error_code(std::int32_t v) { error_code = v; return *this; }
    LogContext& with_latency(std::chrono::microseconds v) { latency = v; return *this; }
    LogContext& with_request_id(std::string v) { request_id = std::move(v); return *this; }

    // Materialises the set members as LogFields, appending to `out`.
    void append_fields(std::vector<LogField>& out) const;
};

// Structured JSON-lines logger.
//
// Guarantees:
//   * One line per record, always valid JSON (all keys and values are
//     escaped — see escape_json_into).
//   * Never emits a secret: the logger cannot know what is sensitive, so the
//     redact*() helpers above are mandatory at the call site, and
//     tests/unit/observability/logger_test.cpp asserts a known secret string
//     never appears in captured output.
//   * The output stream is settable (set_sink) so tests can capture lines
//     without going through stdout/stderr.
class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level) noexcept { level_ = level; }
    [[nodiscard]] LogLevel level() const noexcept { return level_; }

    // Sink receives one complete, newline-terminated JSON line. Passing an
    // empty function restores the default stdout/stderr behaviour. Intended
    // for tests and for embedding; production keeps the default.
    using Sink = std::function<void(LogLevel, std::string_view line)>;
    void set_sink(Sink sink);
    void reset_sink();

    void log(LogLevel level, std::string_view component, std::string_view event,
             std::initializer_list<LogField> fields = {});

    // Named differently from log() on purpose: an overload set containing
    // both initializer_list<LogField> and span<const LogField> is ambiguous
    // for a braced `{}` argument.
    void log_fields(LogLevel level, std::string_view component, std::string_view event,
                    std::span<const LogField> fields);

    // Context-carrying overload: emits worker/connection/stream/application/
    // error_code/latency_us/request_id (whichever are set) followed by the
    // caller's own fields.
    void log(LogLevel level, std::string_view component, std::string_view event, const LogContext& context,
             std::initializer_list<LogField> fields = {});

private:
    Logger() = default;

    void emit(LogLevel level, std::string_view component, std::string_view event, const LogContext* context,
              std::span<const LogField> fields);

    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;
    Sink sink_;
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

// Appends `value` to `out` with JSON string escaping applied (quotes,
// backslashes, and all C0 control characters). Exposed for testing.
void escape_json_into(std::string_view value, std::string& out);

} // namespace rtmp_server::observability

#define RTMP_LOG(level, component, event, ...) \
    rtmp_server::observability::Logger::instance().log(level, component, event, __VA_ARGS__)
