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

void escape_json_into(std::string_view value, std::string& out) {
    out.reserve(out.size() + value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                // All remaining C0 controls must be escaped as \u00XX for the
                // line to be valid JSON. Anything >= 0x20 (including UTF-8
                // continuation bytes) passes through unchanged.
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
                break;
        }
    }
}

std::string redact(std::string_view secret) {
    if (secret.empty()) return "[redacted:empty]";
    // Too short for a prefix to be a meaningful reduction in guessability.
    if (secret.size() < 8) return std::format("[redacted](len={})", secret.size());
    return std::format("{}…(len={})", secret.substr(0, 4), secret.size());
}

std::string redact_fully(std::string_view secret) {
    return secret.empty() ? "[redacted:empty]" : "[redacted]";
}

std::string redact_query(std::string_view uri) {
    const auto question = uri.find('?');
    if (question == std::string_view::npos) return std::string(uri);

    std::string out(uri.substr(0, question + 1));
    std::string_view query = uri.substr(question + 1);

    bool first = true;
    while (!query.empty()) {
        const auto amp = query.find('&');
        std::string_view pair = amp == std::string_view::npos ? query : query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);

        if (!first) out += '&';
        first = false;

        const auto eq = pair.find('=');
        if (eq == std::string_view::npos) {
            // A bare flag carries no credential material; keep it.
            out += pair;
        } else {
            // Keep the parameter NAME (diagnosable) but never its value:
            // token=, sign=, key=, password= all live in this position.
            out += pair.substr(0, eq);
            out += "=[redacted]";
        }
    }
    return out;
}

void LogContext::append_fields(std::vector<LogField>& out) const {
    if (worker_id) out.push_back(LogField{"worker_id", std::format("{}", *worker_id)});
    if (connection_id) out.push_back(LogField{"connection_id", std::format("{}", *connection_id)});
    if (stream_id) out.push_back(LogField{"stream_id", std::format("{}", *stream_id)});
    if (application_id) out.push_back(LogField{"application_id", std::format("{}", *application_id)});
    if (error_code) out.push_back(LogField{"error_code", std::format("{}", *error_code)});
    if (latency) out.push_back(LogField{"latency_us", std::format("{}", latency->count())});
    if (request_id) out.push_back(LogField{"request_id", *request_id});
}

void Logger::set_sink(Sink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::reset_sink() {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = nullptr;
}

void Logger::log(LogLevel level, std::string_view component, std::string_view event,
                 std::initializer_list<LogField> fields) {
    emit(level, component, event, nullptr, std::span<const LogField>(fields.begin(), fields.size()));
}

void Logger::log_fields(LogLevel level, std::string_view component, std::string_view event,
                        std::span<const LogField> fields) {
    emit(level, component, event, nullptr, fields);
}

void Logger::log(LogLevel level, std::string_view component, std::string_view event, const LogContext& context,
                 std::initializer_list<LogField> fields) {
    emit(level, component, event, &context, std::span<const LogField>(fields.begin(), fields.size()));
}

void Logger::emit(LogLevel level, std::string_view component, std::string_view event, const LogContext* context,
                  std::span<const LogField> fields) {
    if (level < level_) return;

    const auto timestamp = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());

    // Build the line before taking the lock: formatting is the expensive part
    // and there is no reason to serialise it across threads. Only the
    // sink write itself needs mutual exclusion (interleaved fputs would tear
    // lines apart).
    std::string line;
    line.reserve(256);
    line += std::format(R"({{"timestamp":"{:%FT%T}Z","level":"{}","component":")", timestamp, to_string(level));
    escape_json_into(component, line);
    line += R"(","event":")";
    escape_json_into(event, line);
    line += '"';

    const auto append_field = [&line](const LogField& field) {
        line += ",\"";
        escape_json_into(field.key, line);
        line += "\":\"";
        escape_json_into(field.value, line);
        line += '"';
    };

    if (context != nullptr) {
        std::vector<LogField> context_fields;
        context_fields.reserve(7);
        context->append_fields(context_fields);
        for (const auto& field : context_fields) append_field(field);
    }
    for (const auto& field : fields) append_field(field);

    line += "}\n";

    std::lock_guard<std::mutex> lock(mutex_);
    if (sink_) {
        sink_(level, line);
        return;
    }
    std::fputs(line.c_str(), level >= LogLevel::Warn ? stderr : stdout);
}

} // namespace rtmp_server::observability
