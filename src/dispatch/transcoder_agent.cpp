#include "rtmp_server/dispatch/transcoder_agent.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace rtmp_server::dispatch {
namespace {

core::Error assignment_error(core::ErrorCode code, std::string message) {
    return core::Error(code, core::ErrorCategory::Configuration, std::move(message));
}

class JsonCursor {
public:
    explicit JsonCursor(std::string_view input) : input_(input) {}

    void whitespace() {
        while (pos_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool take(char expected) {
        whitespace();
        if (pos_ >= input_.size() || input_[pos_] != expected) return false;
        ++pos_;
        return true;
    }

    [[nodiscard]] bool done() {
        whitespace();
        return pos_ == input_.size();
    }

    [[nodiscard]] std::optional<std::string> string() {
        whitespace();
        if (pos_ >= input_.size() || input_[pos_] != '"') return std::nullopt;
        ++pos_;
        std::string out;
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) return std::nullopt;
            const char escaped = input_[pos_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                // Assignment identifiers/URLs do not need JSON unicode
                // escapes. Rejecting them is safer than decoding malformed
                // surrogate pairs differently on origin and agent.
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::uint64_t> unsigned_number() {
        whitespace();
        const auto begin = pos_;
        while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        if (begin == pos_) return std::nullopt;
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(input_.data() + begin, input_.data() + pos_, value);
        if (parsed.ec != std::errc{}) return std::nullopt;
        return value;
    }

    [[nodiscard]] bool skip_value() {
        whitespace();
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '"') return string().has_value();
        if (input_[pos_] == '{') return skip_container('{', '}');
        if (input_[pos_] == '[') return skip_container('[', ']');
        const auto begin = pos_;
        while (pos_ < input_.size() && input_[pos_] != ',' && input_[pos_] != '}' &&
               input_[pos_] != ']') {
            ++pos_;
        }
        while (pos_ > begin &&
               std::isspace(static_cast<unsigned char>(input_[pos_ - 1])) != 0) {
            --pos_;
        }
        return pos_ > begin;
    }

private:
    [[nodiscard]] bool skip_container(char open, char close) {
        if (!take(open)) return false;
        whitespace();
        if (take(close)) return true;
        while (true) {
            if (open == '{') {
                if (!string() || !take(':')) return false;
            }
            if (!skip_value()) return false;
            if (take(close)) return true;
            if (!take(',')) return false;
        }
    }

    std::string_view input_;
    std::size_t pos_ = 0;
};

template <typename Integer>
bool set_number(JsonCursor& cursor, Integer& out) {
    const auto value = cursor.unsigned_number();
    if (!value || *value > std::numeric_limits<Integer>::max()) return false;
    out = static_cast<Integer>(*value);
    return true;
}

bool parse_rendition(JsonCursor& cursor, DispatchedRendition& rendition) {
    if (!cursor.take('{')) return false;
    bool first = true;
    while (true) {
        if (cursor.take('}')) return !first;
        if (!first && !cursor.take(',')) return false;
        first = false;
        const auto key = cursor.string();
        if (!key || !cursor.take(':')) return false;
        if (*key == "name") {
            auto value = cursor.string();
            if (!value) return false;
            rendition.name = std::move(*value);
        } else if (*key == "output_stream") {
            auto value = cursor.string();
            if (!value) return false;
            rendition.output_stream = std::move(*value);
        } else if (*key == "width") {
            if (!set_number(cursor, rendition.width)) return false;
        } else if (*key == "height") {
            if (!set_number(cursor, rendition.height)) return false;
        } else if (*key == "video_bitrate") {
            if (!set_number(cursor, rendition.video_bitrate)) return false;
        } else if (*key == "audio_bitrate") {
            if (!set_number(cursor, rendition.audio_bitrate)) return false;
        } else if (!cursor.skip_value()) {
            return false;
        }
    }
}

bool parse_renditions(JsonCursor& cursor, std::vector<DispatchedRendition>& renditions) {
    if (!cursor.take('[')) return false;
    if (cursor.take(']')) return true;
    while (true) {
        DispatchedRendition rendition;
        if (!parse_rendition(cursor, rendition)) return false;
        renditions.push_back(std::move(rendition));
        if (cursor.take(']')) return true;
        if (!cursor.take(',')) return false;
    }
}

bool valid_component(std::string_view value) {
    if (value.empty() || value.size() > 255 || value == "." || value == "..") return false;
    return std::ranges::all_of(value, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte >= 0x21 && byte != 0x7f && c != '/' && c != '\\' && c != '?' && c != '#';
    });
}

core::Result<void> validate(const TranscoderJobAssignment& assignment) {
    const auto slash = assignment.id.find('/');
    if (slash == std::string::npos || assignment.id.find('/', slash + 1) != std::string::npos ||
        !valid_component(std::string_view(assignment.id).substr(0, slash)) ||
        !valid_component(std::string_view(assignment.id).substr(slash + 1))) {
        return assignment_error(core::ErrorCode::InvalidConfiguration,
                                "id must be application/name");
    }
    if (!assignment.source_url.starts_with("rtmp://") || assignment.source_url.size() > 4096 ||
        std::ranges::any_of(assignment.source_url, [](char c) {
            return static_cast<unsigned char>(c) <= 0x20 || c == 0x7f;
        })) {
        return assignment_error(core::ErrorCode::InvalidConfiguration,
                                "source_url must be an rtmp:// URL");
    }
    const auto application = std::string_view(assignment.id).substr(0, slash);
    const auto job_name = std::string_view(assignment.id).substr(slash + 1);
    if (!valid_component(assignment.target_application) ||
        assignment.target_application != application) {
        return assignment_error(core::ErrorCode::InvalidConfiguration,
                                "target_application must match the id application");
    }
    const bool invalid_origin_host =
        assignment.origin_rtmp_host.empty() || assignment.origin_rtmp_host.size() > 253 ||
        std::ranges::any_of(assignment.origin_rtmp_host, [](char c) {
            return static_cast<unsigned char>(c) <= 0x20 || c == '/' || c == '?' || c == '#' ||
                   c == '@';
        });
    if (invalid_origin_host || assignment.origin_rtmp_port == 0) {
        return assignment_error(core::ErrorCode::InvalidConfiguration,
                                "origin RTMP host and port are required");
    }
    if (assignment.fps == 0 || assignment.fps > 240) {
        return assignment_error(core::ErrorCode::InvalidConfiguration,
                                "fps must be between 1 and 240");
    }
    if (assignment.renditions.empty() || assignment.renditions.size() > 16) {
        return assignment_error(core::ErrorCode::InvalidConfiguration,
                                "between 1 and 16 renditions are required");
    }
    std::unordered_set<std::string> outputs;
    for (const auto& rendition : assignment.renditions) {
        if (!valid_component(rendition.output_stream)) {
            return assignment_error(core::ErrorCode::InvalidConfiguration,
                                    "every output_stream must be a valid path component");
        }
        if (rendition.output_stream == job_name) {
            return assignment_error(core::ErrorCode::InvalidConfiguration,
                                    "an output_stream may not reuse the job name");
        }
        if (!outputs.insert(rendition.output_stream).second) {
            return assignment_error(core::ErrorCode::InvalidConfiguration,
                                    "output_stream values must be unique");
        }
        const auto pixels = static_cast<std::uint64_t>(rendition.width) * rendition.height;
        if (rendition.width > 8192 || rendition.height > 8192 || pixels > 7680u * 4320u ||
            rendition.video_bitrate == 0 || rendition.video_bitrate > 200'000'000 ||
            rendition.audio_bitrate == 0 || rendition.audio_bitrate > 2'000'000) {
            return assignment_error(core::ErrorCode::InvalidConfiguration,
                                    "rendition geometry or bitrate is invalid");
        }
    }
    return {};
}

} // namespace

bool operator==(const TranscoderJobAssignment& lhs, const TranscoderJobAssignment& rhs) {
    if (lhs.id != rhs.id || lhs.source_url != rhs.source_url || lhs.fps != rhs.fps ||
        lhs.target_application != rhs.target_application ||
        lhs.origin_rtmp_host != rhs.origin_rtmp_host ||
        lhs.origin_rtmp_port != rhs.origin_rtmp_port ||
        lhs.renditions.size() != rhs.renditions.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.renditions.size(); ++i) {
        const auto& a = lhs.renditions[i];
        const auto& b = rhs.renditions[i];
        if (a.name != b.name || a.output_stream != b.output_stream || a.width != b.width ||
            a.height != b.height || a.video_bitrate != b.video_bitrate ||
            a.audio_bitrate != b.audio_bitrate) {
            return false;
        }
    }
    return true;
}

core::Result<TranscoderJobAssignment> parse_transcoder_job_assignment(std::string_view json) {
    JsonCursor cursor(json);
    TranscoderJobAssignment assignment;
    if (!cursor.take('{')) {
        return assignment_error(core::ErrorCode::InvalidConfiguration,
                                "assignment must be a JSON object");
    }
    bool first = true;
    while (true) {
        if (cursor.take('}')) break;
        if (!first && !cursor.take(',')) {
            return assignment_error(core::ErrorCode::InvalidConfiguration, "malformed assignment JSON");
        }
        first = false;
        const auto key = cursor.string();
        if (!key || !cursor.take(':')) {
            return assignment_error(core::ErrorCode::InvalidConfiguration, "malformed assignment JSON");
        }
        if (*key == "id") {
            auto value = cursor.string();
            if (!value) return assignment_error(core::ErrorCode::InvalidConfiguration, "id must be a string");
            assignment.id = std::move(*value);
        } else if (*key == "source_url") {
            auto value = cursor.string();
            if (!value) return assignment_error(core::ErrorCode::InvalidConfiguration, "source_url must be a string");
            assignment.source_url = std::move(*value);
        } else if (*key == "fps") {
            if (!set_number(cursor, assignment.fps)) {
                return assignment_error(core::ErrorCode::InvalidConfiguration, "fps must be an unsigned integer");
            }
        } else if (*key == "target_application") {
            auto value = cursor.string();
            if (!value) return assignment_error(core::ErrorCode::InvalidConfiguration, "target_application must be a string");
            assignment.target_application = std::move(*value);
        } else if (*key == "origin_rtmp_host") {
            auto value = cursor.string();
            if (!value) return assignment_error(core::ErrorCode::InvalidConfiguration, "origin_rtmp_host must be a string");
            assignment.origin_rtmp_host = std::move(*value);
        } else if (*key == "origin_rtmp_port") {
            if (!set_number(cursor, assignment.origin_rtmp_port)) {
                return assignment_error(core::ErrorCode::InvalidConfiguration, "origin_rtmp_port must be an unsigned integer");
            }
        } else if (*key == "renditions") {
            if (!parse_renditions(cursor, assignment.renditions)) {
                return assignment_error(core::ErrorCode::InvalidConfiguration, "renditions must be an array of objects");
            }
        } else if (!cursor.skip_value()) {
            return assignment_error(core::ErrorCode::InvalidConfiguration, "malformed assignment JSON");
        }
    }
    if (!cursor.done()) {
        return assignment_error(core::ErrorCode::InvalidConfiguration,
                                "unexpected data after assignment");
    }
    if (auto valid = validate(assignment); !valid) return valid.error();
    return assignment;
}

TranscoderAgent::TranscoderAgent(RunnerFactory factory, TranscoderAgentOptions options)
    : factory_(std::move(factory)), options_(options) {
    if (!factory_) throw std::invalid_argument("transcoder agent runner factory is required");
    if (options_.max_jobs == 0) throw std::invalid_argument("transcoder agent max_jobs must be positive");
}

TranscoderAgent::~TranscoderAgent() { stop_all(); }

core::Result<TranscoderJobRunnerStatus> TranscoderAgent::upsert(
    TranscoderJobAssignment assignment) {
    if (auto valid = validate(assignment); !valid) return valid.error();
    std::lock_guard mutation_lock(mutation_mutex_);
    std::unique_ptr<TranscoderJob> old;
    {
        std::lock_guard jobs_lock(jobs_mutex_);
        if (stopping_) {
            return assignment_error(core::ErrorCode::InvalidStateTransition,
                                    "transcoder agent is stopping");
        }
        const auto existing = jobs_.find(assignment.id);
        if (existing != jobs_.end() && existing->second.assignment == assignment) {
            return existing->second.runner->status();
        }
        if (existing == jobs_.end() && jobs_.size() >= options_.max_jobs) {
            return assignment_error(core::ErrorCode::ResourceExhausted,
                                    "transcoder agent is at job capacity");
        }
        if (existing != jobs_.end()) {
            old = std::move(existing->second.runner);
            jobs_.erase(existing);
        }
    }
    if (old) old->stop();

    std::unique_ptr<TranscoderJob> runner;
    try {
        runner = factory_(assignment);
    } catch (const std::exception& error) {
        return assignment_error(core::ErrorCode::Unknown,
                                "could not start transcoder job: " + std::string(error.what()));
    }
    if (!runner) {
        return assignment_error(core::ErrorCode::Unknown,
                                "runner factory returned no transcoder job");
    }
    auto status = runner->status();
    const auto id = assignment.id;
    {
        std::lock_guard jobs_lock(jobs_mutex_);
        jobs_.emplace(id, Entry{std::move(assignment), std::move(runner)});
    }
    return status;
}

core::Result<void> TranscoderAgent::remove(std::string_view id) {
    std::lock_guard mutation_lock(mutation_mutex_);
    std::unique_ptr<TranscoderJob> runner;
    {
        std::lock_guard jobs_lock(jobs_mutex_);
        const auto found = jobs_.find(std::string(id));
        if (found == jobs_.end()) {
            return assignment_error(core::ErrorCode::NotFound, "no such transcoder job");
        }
        runner = std::move(found->second.runner);
        jobs_.erase(found);
    }
    runner->stop();
    return {};
}

std::vector<TranscoderJobRunnerStatus> TranscoderAgent::list() const {
    std::lock_guard jobs_lock(jobs_mutex_);
    std::vector<TranscoderJobRunnerStatus> result;
    result.reserve(jobs_.size());
    for (const auto& [id, entry] : jobs_) result.push_back(entry.runner->status());
    std::ranges::sort(result, [](const auto& a, const auto& b) { return a.id < b.id; });
    return result;
}

std::size_t TranscoderAgent::size() const {
    std::lock_guard jobs_lock(jobs_mutex_);
    return jobs_.size();
}

void TranscoderAgent::stop_all() {
    std::lock_guard mutation_lock(mutation_mutex_);
    std::unordered_map<std::string, Entry> jobs;
    {
        std::lock_guard jobs_lock(jobs_mutex_);
        stopping_ = true;
        jobs.swap(jobs_);
    }
    for (auto& [id, entry] : jobs) entry.runner->stop();
}

} // namespace rtmp_server::dispatch
