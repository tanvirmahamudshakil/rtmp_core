#pragma once

#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rtmp_server::observability {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error };

// A single structured field, e.g. {"connection_id", "42"}. Values are
// pre-stringified by the caller to keep the logger free of type templates
// on the hot path.
struct LogField {
    std::string_view key;
    std::string value;
};

// Minimal structured JSON-lines logger. Deliberately does not log full
// tokens/stream keys/secrets — callers must redact before passing fields in,
// per docs/rtmp_promot.md "Structured Logging".
class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level) noexcept { level_ = level; }
    [[nodiscard]] LogLevel level() const noexcept { return level_; }

    void log(LogLevel level, std::string_view component, std::string_view event,
              std::initializer_list<LogField> fields = {});

private:
    Logger() = default;

    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

} // namespace rtmp_server::observability

#define RTMP_LOG(level, component, event, ...) \
    rtmp_server::observability::Logger::instance().log(level, component, event, __VA_ARGS__)
