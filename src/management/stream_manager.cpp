#include "rtmp_server/management/stream_manager.hpp"

#include "rtmp_server/core/hmac.hpp"
#include "rtmp_server/core/random.hpp"
#include "rtmp_server/management/token.hpp"
#include "rtmp_server/management/url_builder.hpp"

namespace rtmp_server::management {

using core::Error;
using core::ErrorCategory;
using core::ErrorCode;
using core::Result;

StreamManager::StreamManager(Options options) : options_(std::move(options)) {}

void StreamManager::set_store(persistence::Store* store) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_ = store;
}

void StreamManager::set_audit_log(observability::AuditLog* audit_log) {
    std::lock_guard<std::mutex> lock(mutex_);
    audit_log_ = audit_log;
}

void StreamManager::set_metrics(observability::Metrics* metrics) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_ = metrics;
}

void StreamManager::audit_locked(std::string_view action, std::string_view application, std::string_view name,
                                  bool success, std::string_view detail) {
    if (audit_log_ == nullptr) return;
    observability::AuditEntry entry;
    entry.timestamp = core::wall_now();
    entry.actor = "management-api";
    entry.action = std::string(action);
    entry.application = std::string(application);
    entry.stream_name = std::string(name);
    entry.success = success;
    entry.detail = std::string(detail);
    audit_log_->record(std::move(entry));
}

void StreamManager::count_locked(std::string_view action) {
    if (metrics_ == nullptr) return;
    // Underscore, not dot: a '.' is not a legal character in a Prometheus
    // metric name, so the pre-Phase-7 "management.<action>_total" names were
    // rejected by any scraper reading this process's /metrics endpoint (which
    // advertises text/plain; version=0.0.4). Metrics::is_valid_dynamic_name
    // now enforces this mechanically.
    metrics_->increment_counter("management_" + std::string(action) + "_total");
}

core::Result<void> StreamManager::load_from_store() {
    persistence::Store* store;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        store = store_;
    }
    if (store == nullptr) return Result<void>{};

    auto apps = store->load_applications();
    if (!apps.ok()) return apps.error();
    auto streams = store->load_streams();
    if (!streams.ok()) return streams.error();

    std::lock_guard<std::mutex> lock(mutex_);
    applications_.clear();
    for (const auto& row : apps.value()) {
        applications_[row.name] = ApplicationRecord{row.enabled, {}};
    }
    for (const auto& row : streams.value()) {
        auto app_it = applications_.find(row.application);
        if (app_it == applications_.end()) continue; // orphaned row — application was deleted
        StreamRecord record;
        record.meta.application = row.application;
        record.meta.name = row.name;
        record.meta.enabled = row.enabled;
        record.meta.recording_enabled = row.recording_enabled;
        record.meta.created_at = core::WallClock::time_point(std::chrono::seconds(row.created_at_unix));
        record.key_hash = row.key_hash;
        app_it->second.streams[row.name] = std::move(record);
    }
    return Result<void>{};
}

core::Result<void> StreamManager::create_application(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (applications_.contains(name)) {
        audit_locked("create_application", name, "", false, "already exists");
        return Error(ErrorCode::Conflict, ErrorCategory::Internal, "application already exists");
    }
    if (store_ != nullptr) {
        auto result = store_->upsert_application(persistence::ApplicationRow{name, true});
        if (!result.ok()) audit_locked("create_application", name, "", false, "store write failed");
    }
    applications_[name] = ApplicationRecord{};
    audit_locked("create_application", name, "", true);
    count_locked("create_application");
    return Result<void>{};
}

core::Result<void> StreamManager::delete_application(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = applications_.find(std::string(name));
    if (it == applications_.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "application not found");
    }
    if (!it->second.streams.empty()) {
        return Error(ErrorCode::Conflict, ErrorCategory::Internal, "application still has streams");
    }
    if (store_ != nullptr) {
        auto result = store_->delete_application(name);
        if (!result.ok()) audit_locked("delete_application", name, "", false, "store write failed");
    }
    applications_.erase(it);
    audit_locked("delete_application", name, "", true);
    count_locked("delete_application");
    return Result<void>{};
}

std::vector<Application> StreamManager::list_applications() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Application> out;
    out.reserve(applications_.size());
    for (const auto& [name, record] : applications_) out.push_back(Application{name, record.enabled});
    return out;
}

std::optional<Application> StreamManager::find_application(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = applications_.find(std::string(name));
    if (it == applications_.end()) return std::nullopt;
    return Application{it->first, it->second.enabled};
}

core::Result<StreamCreationResult> StreamManager::create_stream(std::string_view application, std::string name,
                                                                 bool recording_enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "application not found");
    }
    if (app_it->second.streams.contains(name)) {
        return Error(ErrorCode::Conflict, ErrorCategory::Internal, "stream already exists");
    }

    std::string raw_key = core::generate_secure_token(24);
    StreamRecord record;
    record.meta.application = std::string(application);
    record.meta.name = name;
    record.meta.enabled = true;
    record.meta.recording_enabled = recording_enabled;
    record.meta.created_at = core::wall_now();
    record.key_hash = core::sha256_hex(raw_key);

    StreamCreationResult result;
    result.stream = record.meta;
    result.stream_key = raw_key;
    result.publish_url = build_publish_url(options_.public_hostname, options_.rtmp_port, application, raw_key);
    result.playback_url = build_playback_url(options_.public_hostname, options_.rtmp_port, application, name);
    result.stream.playback_url = result.playback_url;

    if (store_ != nullptr) {
        persistence::StreamRow row;
        row.application = record.meta.application;
        row.name = record.meta.name;
        row.key_hash = record.key_hash;
        row.enabled = record.meta.enabled;
        row.recording_enabled = record.meta.recording_enabled;
        row.created_at_unix =
            std::chrono::duration_cast<std::chrono::seconds>(record.meta.created_at.time_since_epoch()).count();
        auto store_result = store_->upsert_stream(row);
        if (!store_result.ok()) audit_locked("create_stream", application, name, false, "store write failed");
    }
    app_it->second.streams[std::move(name)] = std::move(record);
    audit_locked("create_stream", application, result.stream.name, true);
    count_locked("create_stream");
    return result;
}

core::Result<std::string> StreamManager::rotate_key(std::string_view application, std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "application not found");
    }
    auto stream_it = app_it->second.streams.find(std::string(name));
    if (stream_it == app_it->second.streams.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "stream not found");
    }

    std::string raw_key = core::generate_secure_token(24);
    stream_it->second.key_hash = core::sha256_hex(raw_key);

    if (store_ != nullptr) {
        persistence::StreamRow row;
        row.application = stream_it->second.meta.application;
        row.name = stream_it->second.meta.name;
        row.key_hash = stream_it->second.key_hash;
        row.enabled = stream_it->second.meta.enabled;
        row.recording_enabled = stream_it->second.meta.recording_enabled;
        row.created_at_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                   stream_it->second.meta.created_at.time_since_epoch())
                                   .count();
        auto store_result = store_->upsert_stream(row);
        if (!store_result.ok()) audit_locked("rotate_key", application, name, false, "store write failed");
    }
    audit_locked("rotate_key", application, name, true);
    count_locked("rotate_key");
    return raw_key;
}

core::Result<void> StreamManager::set_enabled(std::string_view application, std::string_view name, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "application not found");
    }
    auto stream_it = app_it->second.streams.find(std::string(name));
    if (stream_it == app_it->second.streams.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "stream not found");
    }
    stream_it->second.meta.enabled = enabled;

    if (store_ != nullptr) {
        persistence::StreamRow row;
        row.application = stream_it->second.meta.application;
        row.name = stream_it->second.meta.name;
        row.key_hash = stream_it->second.key_hash;
        row.enabled = enabled;
        row.recording_enabled = stream_it->second.meta.recording_enabled;
        row.created_at_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                   stream_it->second.meta.created_at.time_since_epoch())
                                   .count();
        store_->upsert_stream(row);
    }
    audit_locked(enabled ? "enable_stream" : "disable_stream", application, name, true);
    count_locked(enabled ? "enable_stream" : "disable_stream");
    return Result<void>{};
}

core::Result<void> StreamManager::set_recording_enabled(std::string_view application, std::string_view name,
                                                         bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "application not found");
    }
    auto stream_it = app_it->second.streams.find(std::string(name));
    if (stream_it == app_it->second.streams.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "stream not found");
    }
    stream_it->second.meta.recording_enabled = enabled;

    if (store_ != nullptr) {
        persistence::StreamRow row;
        row.application = stream_it->second.meta.application;
        row.name = stream_it->second.meta.name;
        row.key_hash = stream_it->second.key_hash;
        row.enabled = stream_it->second.meta.enabled;
        row.recording_enabled = enabled;
        row.created_at_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                   stream_it->second.meta.created_at.time_since_epoch())
                                   .count();
        store_->upsert_stream(row);
    }
    audit_locked("set_recording_enabled", application, name, true);
    count_locked("set_recording_enabled");
    return Result<void>{};
}

core::Result<void> StreamManager::delete_stream(std::string_view application, std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "application not found");
    }
    auto stream_it = app_it->second.streams.find(std::string(name));
    if (stream_it == app_it->second.streams.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "stream not found");
    }
    if (store_ != nullptr) store_->delete_stream(application, name);
    app_it->second.streams.erase(stream_it);
    audit_locked("delete_stream", application, name, true);
    count_locked("delete_stream");
    return Result<void>{};
}

std::optional<Stream> StreamManager::find_stream(std::string_view application, std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) return std::nullopt;
    auto stream_it = app_it->second.streams.find(std::string(name));
    if (stream_it == app_it->second.streams.end()) return std::nullopt;
    Stream stream = stream_it->second.meta;
    stream.playback_url =
        build_playback_url(options_.public_hostname, options_.rtmp_port, stream.application, stream.name);
    return stream;
}

std::vector<Stream> StreamManager::list_streams(std::string_view application) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Stream> out;
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) return out;
    out.reserve(app_it->second.streams.size());
    for (const auto& [name, record] : app_it->second.streams) {
        Stream stream = record.meta;
        stream.playback_url =
            build_playback_url(options_.public_hostname, options_.rtmp_port, stream.application, stream.name);
        out.push_back(std::move(stream));
    }
    return out;
}

bool StreamManager::validate_publish_key(std::string_view application, std::string_view raw_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end() || !app_it->second.enabled) return false;

    std::string candidate_hash = core::sha256_hex(raw_key);
    for (const auto& [name, record] : app_it->second.streams) {
        if (!record.meta.enabled) continue;
        if (core::constant_time_equals(record.key_hash, candidate_hash)) return true;
    }
    return false;
}

std::optional<std::string> StreamManager::resolve_stream_name_for_key(std::string_view application,
                                                                        std::string_view raw_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end() || !app_it->second.enabled) return std::nullopt;

    std::string candidate_hash = core::sha256_hex(raw_key);
    for (const auto& [name, record] : app_it->second.streams) {
        if (!record.meta.enabled) continue;
        if (core::constant_time_equals(record.key_hash, candidate_hash)) return name;
    }
    return std::nullopt;
}

std::string StreamManager::sign_playback_token(std::string_view application, std::string_view name,
                                                std::int64_t expires_at_unix) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sign_token(options_.token_signing_secret, application, name, expires_at_unix);
}

core::Result<void> StreamManager::verify_playback_token(std::string_view application, std::string_view name,
                                                         std::string_view token, std::int64_t expires_at_unix,
                                                         std::int64_t now_unix) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "application not found");
    }
    auto stream_it = app_it->second.streams.find(std::string(name));
    if (stream_it == app_it->second.streams.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "stream not found");
    }
    if (!stream_it->second.meta.enabled) {
        return Error(ErrorCode::Unauthorized, ErrorCategory::Authentication, "stream disabled");
    }
    return verify_token(options_.token_signing_secret, application, name, token, expires_at_unix, now_unix);
}

void StreamManager::set_publisher_disconnect_handler(PublisherDisconnectHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    publisher_disconnect_handler_ = std::move(handler);
}

void StreamManager::set_viewer_disconnect_handler(ViewerDisconnectHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    viewer_disconnect_handler_ = std::move(handler);
}

std::optional<std::string> StreamManager::resolve_live_key_locked(
    const std::string& application, const StreamRecord& record,
    const protocol::commands::StreamRegistry& registry) const {
    for (const auto& reg : registry.snapshot()) {
        if (reg.app != application) continue;
        // Open production mode registers the public stream name directly.
        // Retain the legacy hash comparison for optional authenticated
        // embedders.
        if (reg.stream_key == record.meta.name ||
            core::constant_time_equals(core::sha256_hex(reg.stream_key), record.key_hash)) {
            return reg.stream_key;
        }
    }
    return std::nullopt;
}

core::Result<void> StreamManager::disconnect_publisher(std::string_view application, std::string_view name,
                                                        const protocol::commands::StreamRegistry& registry) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "application not found");
    }
    auto stream_it = app_it->second.streams.find(std::string(name));
    if (stream_it == app_it->second.streams.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "stream not found");
    }

    for (const auto& reg : registry.snapshot()) {
        if (reg.app != application) continue;
        if (reg.stream_key != stream_it->second.meta.name &&
            !core::constant_time_equals(core::sha256_hex(reg.stream_key), stream_it->second.key_hash)) {
            continue;
        }
        if (publisher_disconnect_handler_) publisher_disconnect_handler_(reg.connection_id);
        audit_locked("disconnect_publisher", application, name, true);
        count_locked("disconnect_publisher");
        return Result<void>{};
    }
    audit_locked("disconnect_publisher", application, name, false, "not currently published");
    return Error(ErrorCode::NotFound, ErrorCategory::Internal, "stream is not currently being published");
}

core::Result<void> StreamManager::disconnect_viewers(std::string_view application, std::string_view name,
                                                      const protocol::commands::StreamRegistry& registry) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto app_it = applications_.find(std::string(application));
    if (app_it == applications_.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "application not found");
    }
    auto stream_it = app_it->second.streams.find(std::string(name));
    if (stream_it == app_it->second.streams.end()) {
        return Error(ErrorCode::NotFound, ErrorCategory::Internal, "stream not found");
    }

    for (const auto& reg : registry.snapshot()) {
        if (reg.app != application) continue;
        if (reg.stream_key != stream_it->second.meta.name &&
            !core::constant_time_equals(core::sha256_hex(reg.stream_key), stream_it->second.key_hash)) {
            continue;
        }
        if (viewer_disconnect_handler_) viewer_disconnect_handler_(reg.stream_key);
        audit_locked("disconnect_viewers", application, name, true);
        count_locked("disconnect_viewers");
        return Result<void>{};
    }
    audit_locked("disconnect_viewers", application, name, false, "not currently live");
    return Error(ErrorCode::NotFound, ErrorCategory::Internal, "stream is not currently live");
}

std::vector<LiveState> StreamManager::live_state(const protocol::commands::StreamRegistry& registry,
                                                  const protocol::commands::LiveFanout& fanout,
                                                  const protocol::commands::StreamIdRegistry& stream_ids) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LiveState> out;
    for (const auto& [app_name, app_record] : applications_) {
        for (const auto& [stream_name, record] : app_record.streams) {
            LiveState state;
            state.application = app_name;
            state.name = stream_name;
            auto raw_key = resolve_live_key_locked(app_name, record, registry);
            state.is_live = raw_key.has_value();
            state.viewer_count = 0;
            if (raw_key) {
                auto id = stream_ids.find(app_name, *raw_key);
                // Unique by client IP, not raw connection count: the same
                // viewer reloading or opening a second tab on this link
                // should not inflate the count the admin panel shows.
                if (id) state.viewer_count = fanout.unique_viewer_count(*id);
            }
            out.push_back(std::move(state));
        }
    }
    return out;
}

} // namespace rtmp_server::management
