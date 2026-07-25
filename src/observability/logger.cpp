#include "rtmp_server/observability/logger.hpp"

#include <chrono>
#include <cstdio>
#include <format>

namespace rtmp_server::observability {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "unknown";
}

void Logger::log(LogLevel level, std::string_view component, std::string_view event,
                  std::initializer_list<LogField> fields) {
    if (level < level_) return;

    const auto now = std::chrono::system_clock::now();
    const auto timestamp =
        std::chrono::floor<std::chrono::milliseconds>(now);

    std::lock_guard<std::mutex> lock(mutex_);

    std::string line = std::format(
        R"({{"timestamp":"{:%FT%T}Z","level":"{}","component":"{}","event":"{}")",
        timestamp, to_string(level), component, event);

    for (const auto& field : fields) {
        line += std::format(R"(,"{}":"{}")", field.key, field.value);
    }
    line += "}\n";

    std::fputs(line.c_str(), level >= LogLevel::Warn ? stderr : stdout);
}

} // namespace rtmp_server::observability
