#include "rtmp_server/relay/stream_target_manager.hpp"

#include <algorithm>
#include <utility>

#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/protocol/rtmp_url.hpp"

namespace rtmp_server::relay {
namespace {

core::Error target_error(core::ErrorCode code, std::string message) {
    return core::Error(code, core::ErrorCategory::Configuration, std::move(message));
}

// Forwards one publish to several targets. Kept private to the manager: the
// publish path only ever sees a RecorderSink.
class TargetGroupSink final : public protocol::commands::RecorderSink {
public:
    explicit TargetGroupSink(std::vector<std::shared_ptr<StreamTargetSink>> sinks)
        : sinks_(std::move(sinks)) {}

    void on_metadata(const protocol::chunk::RtmpMessage& message) override {
        for (auto& sink : sinks_) sink->on_metadata(message);
    }
    void on_audio(const protocol::chunk::RtmpMessage& message) override {
        for (auto& sink : sinks_) sink->on_audio(message);
    }
    void on_video(const protocol::chunk::RtmpMessage& message) override {
        for (auto& sink : sinks_) sink->on_video(message);
    }
    void finalize() override {
        for (auto& sink : sinks_) sink->finalize();
    }

private:
    std::vector<std::shared_ptr<StreamTargetSink>> sinks_;
};

} // namespace

StreamTargetManager::StreamTargetManager(persistence::Store* store, Options options)
    : store_(store), options_(options) {}

StreamTargetManager::~StreamTargetManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, state] : streams_) {
        for (auto& [name, sink] : state.sinks) sink->finalize();
    }
}

std::string StreamTargetManager::key_of(std::string_view application, std::string_view stream) {
    return std::string(application) + "/" + std::string(stream);
}

void StreamTargetManager::load_from_store() {
    if (store_ == nullptr) return;
    auto rows = store_->load_stream_targets();
    if (!rows) {
        RTMP_LOG(observability::LogLevel::Warn, "stream-target", "target load failed",
                 {{"error", rows.error().message()}});
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& row : rows.value()) {
        if (!protocol::parse_rtmp_url(row.url)) {
            RTMP_LOG(observability::LogLevel::Warn, "stream-target", "target skipped",
                     {{"application", row.application},
                      {"stream", row.stream},
                      {"target", row.name},
                      {"error", "stored URL is not a valid rtmp:// target"}});
            continue;
        }
        StreamTargetConfig config;
        config.application = row.application;
        config.stream = row.stream;
        config.name = row.name;
        config.url = row.url;
        config.enabled = row.enabled;
        config.relay = row.relay;
        config.restart_delay_seconds = options_.restart_delay_seconds;
        targets_[key_of(row.application, row.stream)][row.name] = std::move(config);
    }
}

StreamTargetStatus StreamTargetManager::status_for_locked(const StreamTargetConfig& config) const {
    const auto stream = streams_.find(key_of(config.application, config.stream));
    if (stream != streams_.end()) {
        const auto sink = stream->second.sinks.find(config.name);
        if (sink != stream->second.sinks.end()) return sink->second->status();
    }
    // No publisher: report the configuration, not a live connection.
    StreamTargetStatus status;
    status.application = config.application;
    status.stream = config.stream;
    status.name = config.name;
    status.url_redacted = redact_rtmp_url(config.url);
    status.relay = config.relay;
    status.enabled = config.enabled;
    status.state = StreamTargetState::Stopped;
    status.detail = config.enabled ? "waiting for a publisher" : "disabled";
    return status;
}

void StreamTargetManager::stop_locked(const std::string& key, std::string_view name) {
    const auto stream = streams_.find(key);
    if (stream == streams_.end()) return;
    const auto sink = stream->second.sinks.find(std::string(name));
    if (sink == stream->second.sinks.end()) return;
    sink->second->finalize();
    stream->second.sinks.erase(sink);
}

core::Result<StreamTargetStatus> StreamTargetManager::upsert(const StreamTargetConfig& config) {
    if (config.application.empty() || config.stream.empty() || config.name.empty()) {
        return target_error(core::ErrorCode::InvalidConfiguration,
                            "application, stream and target name are required");
    }
    if (config.name.size() > 128) {
        return target_error(core::ErrorCode::InvalidConfiguration, "target name is too long");
    }
    auto parsed = protocol::parse_rtmp_url(config.url);
    if (!parsed) return parsed.error();

    StreamTargetConfig stored = config;
    stored.restart_delay_seconds =
        config.restart_delay_seconds == 0 ? options_.restart_delay_seconds
                                          : config.restart_delay_seconds;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = key_of(config.application, config.stream);
        auto& by_name = targets_[key];
        if (!by_name.contains(stored.name) && by_name.size() >= kMaxTargetsPerStream) {
            return target_error(core::ErrorCode::ResourceExhausted,
                                "too many targets for one stream");
        }
        // A changed URL or a disabled target invalidates the connection that is
        // running now; the next publish (or the enable below) starts a fresh
        // one. Reconfiguring a live push in place would mean reconnecting
        // anyway.
        stop_locked(key, stored.name);
        by_name[stored.name] = stored;
    }

    if (store_ != nullptr) {
        persistence::StreamTargetRow row;
        row.application = stored.application;
        row.stream = stored.stream;
        row.name = stored.name;
        row.url = stored.url;
        row.enabled = stored.enabled;
        row.relay = stored.relay;
        if (auto saved = store_->upsert_stream_target(row); !saved) return saved.error();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return status_for_locked(stored);
}

core::Result<void> StreamTargetManager::remove(std::string_view application,
                                               std::string_view stream, std::string_view name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = key_of(application, stream);
        const auto targets = targets_.find(key);
        if (targets == targets_.end() || targets->second.erase(std::string(name)) == 0) {
            return target_error(core::ErrorCode::NotFound, "no such stream target");
        }
        stop_locked(key, name);
        if (targets->second.empty()) targets_.erase(targets);
    }
    if (store_ != nullptr) {
        if (auto deleted = store_->delete_stream_target(application, stream, name); !deleted) {
            return deleted.error();
        }
    }
    return {};
}

core::Result<StreamTargetStatus> StreamTargetManager::set_enabled(std::string_view application,
                                                                   std::string_view stream,
                                                                   std::string_view name,
                                                                   bool enabled) {
    StreamTargetConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = key_of(application, stream);
        const auto targets = targets_.find(key);
        if (targets == targets_.end()) return target_error(core::ErrorCode::NotFound, "no such stream target");
        const auto target = targets->second.find(std::string(name));
        if (target == targets->second.end()) {
            return target_error(core::ErrorCode::NotFound, "no such stream target");
        }
        target->second.enabled = enabled;
        config = target->second;
        // Disabling takes the push down now rather than at the next publish.
        // Enabling attaches at the next publish: this manager does not hold the
        // publisher's media path and cannot join one already in progress.
        if (!enabled) stop_locked(key, name);
    }
    if (store_ != nullptr) {
        persistence::StreamTargetRow row;
        row.application = config.application;
        row.stream = config.stream;
        row.name = config.name;
        row.url = config.url;
        row.enabled = config.enabled;
        row.relay = config.relay;
        if (auto saved = store_->upsert_stream_target(row); !saved) return saved.error();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return status_for_locked(config);
}

std::vector<StreamTargetStatus> StreamTargetManager::list(std::string_view application) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<StreamTargetStatus> result;
    for (const auto& [key, by_name] : targets_) {
        for (const auto& [name, config] : by_name) {
            if (!application.empty() && config.application != application) continue;
            result.push_back(status_for_locked(config));
        }
    }
    std::ranges::sort(result, [](const StreamTargetStatus& a, const StreamTargetStatus& b) {
        return std::tie(a.application, a.stream, a.name) < std::tie(b.application, b.stream, b.name);
    });
    return result;
}

std::size_t StreamTargetManager::active_target_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& [key, state] : streams_) {
        for (const auto& [name, sink] : state.sinks) {
            if (sink->status().state == StreamTargetState::Publishing) ++count;
        }
    }
    return count;
}

std::shared_ptr<protocol::commands::RecorderSink> StreamTargetManager::create_sink(
    std::string_view application, std::string_view stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = key_of(application, stream);
    const auto targets = targets_.find(key);
    if (targets == targets_.end()) return nullptr;

    auto& state = streams_[key];
    // A publisher that reconnects before the previous connection was torn down
    // would otherwise leave two pushes writing to the same destination.
    for (auto& [name, sink] : state.sinks) sink->finalize();
    state.sinks.clear();

    std::vector<std::shared_ptr<StreamTargetSink>> sinks;
    for (const auto& [name, config] : targets->second) {
        if (!config.enabled) continue;
        auto sink = std::make_shared<StreamTargetSink>(config, options_.queue_limits);
        state.sinks.emplace(name, sink);
        sinks.push_back(std::move(sink));
    }
    if (sinks.empty()) {
        streams_.erase(key);
        return nullptr;
    }
    return std::make_shared<TargetGroupSink>(std::move(sinks));
}

void StreamTargetManager::release(std::string_view application, std::string_view stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto state = streams_.find(key_of(application, stream));
    if (state == streams_.end()) return;
    for (auto& [name, sink] : state->second.sinks) sink->finalize();
    streams_.erase(state);
}

} // namespace rtmp_server::relay
