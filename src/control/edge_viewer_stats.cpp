#include "rtmp_server/control/edge_viewer_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace rtmp_server::control {

namespace {

// A deliberately small JSON reader, not a general one. The only document it
// ever sees is written by deploy/viewer-estimator/viewer_estimator.py via
// json.dump(), so this handles exactly that grammar: objects, strings with
// standard escapes, numbers, and enough value-skipping to step over keys it
// does not care about. Anything malformed aborts the parse and the caller
// treats the reading as unavailable — never as zero viewers.
class JsonCursor {
public:
    explicit JsonCursor(std::string_view text) : text_(text) {}

    void skip_whitespace() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    [[nodiscard]] bool eof() {
        skip_whitespace();
        return pos_ >= text_.size();
    }

    [[nodiscard]] char peek() {
        skip_whitespace();
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    bool consume(char expected) {
        skip_whitespace();
        if (pos_ >= text_.size() || text_[pos_] != expected) return false;
        ++pos_;
        return true;
    }

    // Reads a JSON string, decoding escapes. \uXXXX is emitted as UTF-8;
    // surrogate pairs are joined so a non-ASCII stream name (which Python
    // escapes by default) round-trips to the same bytes the origin used.
    bool read_string(std::string& out) {
        out.clear();
        if (!consume('"')) return false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return false;
            const char escape = text_[pos_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t code = 0;
                    if (!read_hex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        // High surrogate: a low surrogate must follow.
                        if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                            text_[pos_ + 1] != 'u') {
                            return false;
                        }
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!read_hex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return false; // lone low surrogate
                    }
                    append_utf8(out, code);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool read_number(double& out) {
        skip_whitespace();
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) ||
                                       text_[pos_] == '.' || text_[pos_] == 'e' ||
                                       text_[pos_] == 'E' || text_[pos_] == '-' ||
                                       text_[pos_] == '+')) {
            ++pos_;
        }
        if (pos_ == start) return false;
        const std::string token(text_.substr(start, pos_ - start));
        try {
            std::size_t consumed = 0;
            out = std::stod(token, &consumed);
            return consumed == token.size() && std::isfinite(out);
        } catch (...) {
            return false;
        }
    }

    // Steps over any value, so an unrecognised key costs nothing.
    bool skip_value(int depth = 0) {
        if (depth > 32) return false; // the real document is two levels deep
        skip_whitespace();
        if (pos_ >= text_.size()) return false;
        const char c = text_[pos_];
        if (c == '"') {
            std::string discarded;
            return read_string(discarded);
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++pos_;
            skip_whitespace();
            if (consume(close)) return true;
            for (;;) {
                if (c == '{') {
                    std::string key;
                    if (!read_string(key)) return false;
                    if (!consume(':')) return false;
                }
                if (!skip_value(depth + 1)) return false;
                if (consume(',')) continue;
                return consume(close);
            }
        }
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return true;
        }
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return true;
        }
        double discarded = 0;
        return read_number(discarded);
    }

private:
    bool read_hex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

// A count is a non-negative integer. Anything else (negative, NaN, absurdly
// large) is dropped rather than clamped, so a corrupted field cannot invent
// viewers.
bool to_count(double value, std::uint64_t& out) {
    if (!std::isfinite(value) || value < 0) return false;
    constexpr double kMaximum = 1e18;
    if (value > kMaximum) return false;
    out = static_cast<std::uint64_t>(value);
    return true;
}

bool read_count_map(JsonCursor& cursor, std::unordered_map<std::string, std::uint64_t>& out) {
    if (!cursor.consume('{')) return false;
    if (cursor.consume('}')) return true;
    for (;;) {
        std::string key;
        if (!cursor.read_string(key)) return false;
        if (!cursor.consume(':')) return false;
        double raw = 0;
        if (!cursor.read_number(raw)) return false;
        std::uint64_t count = 0;
        if (to_count(raw, count)) out.emplace(std::move(key), count);
        if (cursor.consume(',')) continue;
        return cursor.consume('}');
    }
}

bool read_totals(JsonCursor& cursor, EdgeViewerSnapshot& snapshot) {
    if (!cursor.consume('{')) return false;
    if (cursor.consume('}')) return true;
    for (;;) {
        std::string key;
        if (!cursor.read_string(key)) return false;
        if (!cursor.consume(':')) return false;
        double raw = 0;
        std::uint64_t count = 0;
        if (key == "viewers" || key == "bytes_total" || key == "bitrate_bps") {
            if (!cursor.read_number(raw)) return false;
            if (to_count(raw, count)) {
                if (key == "viewers") snapshot.total_viewers = count;
                else if (key == "bytes_total") snapshot.total_bytes = count;
                else snapshot.total_bitrate_bps = count;
            }
        } else if (!cursor.skip_value()) {
            return false;
        }
        if (cursor.consume(',')) continue;
        return cursor.consume('}');
    }
}

std::uint64_t sum_for(const std::unordered_map<std::string, std::uint64_t>& values,
                      std::string_view application, std::span<const std::string> streams) {
    std::uint64_t total = 0;
    for (const auto& stream : streams) {
        std::string key;
        key.reserve(application.size() + stream.size() + 1);
        key.append(application);
        key.push_back('/');
        key.append(stream);
        if (const auto it = values.find(key); it != values.end()) total += it->second;
    }
    return total;
}

} // namespace

EdgeViewerSnapshot parse_edge_viewer_stats(std::string_view document,
                                           std::chrono::system_clock::time_point now,
                                           std::chrono::seconds staleness_grace) {
    EdgeViewerSnapshot snapshot;
    JsonCursor cursor(document);
    if (!cursor.consume('{')) return snapshot;

    double generated_at = 0;
    bool have_generated_at = false;
    double window_seconds = 20;

    if (!cursor.consume('}')) {
        for (;;) {
            std::string key;
            if (!cursor.read_string(key)) return EdgeViewerSnapshot{};
            if (!cursor.consume(':')) return EdgeViewerSnapshot{};

            bool ok = true;
            if (key == "generated_at") {
                ok = cursor.read_number(generated_at);
                have_generated_at = ok;
            } else if (key == "window_seconds") {
                ok = cursor.read_number(window_seconds);
            } else if (key == "viewers") {
                ok = read_count_map(cursor, snapshot.viewers);
            } else if (key == "bytes_total") {
                ok = read_count_map(cursor, snapshot.bytes_total);
            } else if (key == "bitrate_bps") {
                ok = read_count_map(cursor, snapshot.bitrate_bps);
            } else if (key == "totals") {
                ok = read_totals(cursor, snapshot);
            } else {
                ok = cursor.skip_value();
            }
            if (!ok) return EdgeViewerSnapshot{};

            if (cursor.consume(',')) continue;
            if (!cursor.consume('}')) return EdgeViewerSnapshot{};
            break;
        }
    }

    if (!have_generated_at) return EdgeViewerSnapshot{};

    // The estimator rewrites the file every two seconds. Anything older than
    // its own measurement window plus a grace period describes viewers who
    // may all have left; report it as unavailable so callers fall back
    // instead of showing a frozen number.
    const double now_seconds =
        std::chrono::duration<double>(now.time_since_epoch()).count();
    const double age = std::abs(now_seconds - generated_at);
    const double allowed =
        std::max(30.0, (window_seconds > 0 ? window_seconds : 20.0) +
                           static_cast<double>(staleness_grace.count()));
    if (age > allowed) return EdgeViewerSnapshot{};

    // The estimator always publishes deduplicated totals; derive them only
    // as a fallback for an older writer that did not.
    if (snapshot.total_viewers == 0) {
        for (const auto& [key, value] : snapshot.viewers) {
            (void)key;
            snapshot.total_viewers += value;
        }
    }
    if (snapshot.total_bytes == 0) {
        for (const auto& [key, value] : snapshot.bytes_total) {
            (void)key;
            snapshot.total_bytes += value;
        }
    }
    if (snapshot.total_bitrate_bps == 0) {
        for (const auto& [key, value] : snapshot.bitrate_bps) {
            (void)key;
            snapshot.total_bitrate_bps += value;
        }
    }

    snapshot.fresh = true;
    return snapshot;
}

EdgeViewerStats::EdgeViewerStats(Options options) : options_(std::move(options)) {}

void EdgeViewerStats::refresh_locked() {
    cached_ = EdgeViewerSnapshot{};
    if (options_.path.empty()) return;

    std::ifstream file(options_.path, std::ios::binary);
    if (!file) return;

    std::string document;
    document.resize(options_.max_bytes);
    file.read(document.data(), static_cast<std::streamsize>(document.size()));
    const auto read_bytes = static_cast<std::size_t>(file.gcount());
    // A document at the ceiling was almost certainly truncated mid-parse;
    // treat it as unusable rather than parsing a partial object.
    if (read_bytes == 0 || read_bytes >= options_.max_bytes) return;
    document.resize(read_bytes);

    cached_ = parse_edge_viewer_stats(document, std::chrono::system_clock::now(),
                                      options_.staleness_grace);
}

EdgeViewerSnapshot EdgeViewerStats::snapshot() {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!ever_read_ || now - last_read_ >= options_.refresh_interval) {
        refresh_locked();
        last_read_ = now;
        ever_read_ = true;
    }
    return cached_;
}

std::uint64_t EdgeViewerStats::viewers_for(std::string_view application, std::string_view stream) {
    const std::string streams[] = {std::string(stream)};
    return viewers_for_any(application, streams);
}

std::uint64_t EdgeViewerStats::viewers_for_any(std::string_view application,
                                               std::span<const std::string> streams) {
    const auto reading = snapshot();
    if (!reading.fresh) return 0;
    return sum_for(reading.viewers, application, streams);
}

std::uint64_t EdgeViewerStats::bytes_for_any(std::string_view application,
                                             std::span<const std::string> streams) {
    const auto reading = snapshot();
    if (!reading.fresh) return 0;
    return sum_for(reading.bytes_total, application, streams);
}

std::uint64_t EdgeViewerStats::bitrate_for_any(std::string_view application,
                                               std::span<const std::string> streams) {
    const auto reading = snapshot();
    if (!reading.fresh) return 0;
    return sum_for(reading.bitrate_bps, application, streams);
}

} // namespace rtmp_server::control
