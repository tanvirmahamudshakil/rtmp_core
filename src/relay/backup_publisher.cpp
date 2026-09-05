#include "rtmp_server/relay/backup_publisher.hpp"

#include <algorithm>
#include <condition_variable>
#include <utility>

#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/protocol/rtmp_url.hpp"
#include "rtmp_server/relay/stream_target.hpp" // redact_rtmp_url
#include "rtmp_server/transcoding/native/rtmp_source_client.hpp"

namespace rtmp_server::relay {
namespace {

using transcoding::native::RtmpSourceClient;

core::Error config_error(std::string message) {
    return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Configuration,
                       std::move(message));
}

core::Error not_found_error(std::string message) {
    return core::Error(core::ErrorCode::NotFound, core::ErrorCategory::Configuration,
                       std::move(message));
}

} // namespace

// One stream's backup worker. Owns a thread that sits idle in Standby and, the
// moment activation is requested, connects to the backup source and forwards
// its media directly into the packaging sink -- the backup source is plain
// RTMP, the same shape a real publisher's connection produces, so no
// conversion is needed (contrast the ingest transcode ladder, which decodes
// and re-encodes; this is pure passthrough).
class BackupPublisherManager::Worker {
public:
    Worker(BackupPublisherConfig config, SinkFactory make_sink)
        : config_(std::move(config)), make_sink_(std::move(make_sink)) {
        thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
    }

    ~Worker() {
        stop_requested_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_requested_ = false;
        }
        cv_.notify_all();
    }

    void update_config(BackupPublisherConfig config) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = std::move(config);
    }

    void set_active(bool active) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_requested_ == active) return;
            active_requested_ = active;
        }
        cv_.notify_all();
    }

    [[nodiscard]] BackupPublisherStatus status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        BackupPublisherStatus status;
        status.application = config_.application;
        status.stream = config_.stream;
        status.url_redacted = redact_rtmp_url(config_.backup_url);
        status.enabled = config_.enabled;
        status.state = state_;
        status.detail = detail_;
        status.activations = activations_;
        status.bytes_in = bytes_in_;
        return status;
    }

private:
    void set_detail_locked(std::string detail) { detail_ = std::move(detail); }

    void run(const std::stop_token& stop) {
        while (!stop.stop_requested() && !stop_requested_.load()) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] {
                    return active_requested_ || stop.stop_requested() || stop_requested_.load();
                });
                if (stop.stop_requested() || stop_requested_.load()) break;
                if (!active_requested_) continue;
                state_ = BackupPublisherState::Connecting;
                set_detail_locked("connecting to backup source");
            }

            BackupPublisherConfig config;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                config = config_;
            }

            auto sink = make_sink_(config.application, config.stream);
            if (!sink) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = BackupPublisherState::Error;
                set_detail_locked("packaging sink unavailable");
                active_requested_ = false;
                continue;
            }

            RtmpSourceClient client(config.backup_url);
            bool announced_active = false;
            auto result = client.run(
                [this] {
                    std::lock_guard<std::mutex> lock(mutex_);
                    return active_requested_ && !stop_requested_.load();
                },
                [&](const protocol::chunk::RtmpMessage& message) -> core::Result<void> {
                    using protocol::chunk::MessageTypeId;
                    const auto type = static_cast<MessageTypeId>(message.message_type_id);
                    if (type == MessageTypeId::Video) {
                        sink->on_video(message);
                    } else if (type == MessageTypeId::Audio) {
                        sink->on_audio(message);
                    } else if (type == MessageTypeId::Amf0Data || type == MessageTypeId::Amf3Data) {
                        sink->on_metadata(message);
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        bytes_in_ += message.payload.size();
                    }
                    return {};
                },
                [&] {
                    announced_active = true;
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_ = BackupPublisherState::Active;
                    set_detail_locked("active: serving from backup source");
                    ++activations_;
                });

            sink->finalize();

            std::unique_lock<std::mutex> lock(mutex_);
            const bool still_wanted = active_requested_ && !stop_requested_.load();
            if (!still_wanted) {
                // Deactivated because the primary came back, or the manager is
                // shutting this backup down -- not a failure.
                state_ = BackupPublisherState::Standby;
                set_detail_locked("standby: primary is live");
                continue;
            }
            if (!result) {
                state_ = BackupPublisherState::Error;
                set_detail_locked(result.error().message());
                RTMP_LOG(observability::LogLevel::Warn, "backup-publisher", "backup source failed",
                         {{"application", config.application},
                          {"stream", config.stream},
                          {"error", result.error().message()}});
            }
            (void)announced_active;

            // Retry after the configured delay, unless deactivated meanwhile.
            const auto delay = std::chrono::seconds(std::max<std::uint32_t>(config.restart_delay_seconds, 1));
            cv_.wait_for(lock, delay, [&] { return !active_requested_ || stop_requested_.load(); });
        }
    }

    BackupPublisherConfig config_;
    SinkFactory make_sink_;
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool active_requested_ = false;
    BackupPublisherState state_ = BackupPublisherState::Standby;
    std::string detail_ = "standby: primary is live";
    std::uint64_t activations_ = 0;
    std::uint64_t bytes_in_ = 0;

    // Declared last so it is destroyed (and joined) FIRST: member destruction
    // runs in reverse declaration order, and the worker thread reads/locks
    // mutex_/cv_ for its entire life. Declaring the thread any earlier would
    // destroy the mutex out from under a thread that has not been joined yet
    // -- a real crash (EINVAL from a locked-but-destroyed mutex), not a
    // theoretical one.
    std::jthread thread_;
};

BackupPublisherManager::BackupPublisherManager(persistence::Store* store, IsPublisherLive is_live,
                                               SinkFactory make_sink)
    : store_(store), is_live_(std::move(is_live)), make_sink_(std::move(make_sink)) {
    monitor_thread_ = std::jthread([this](const std::stop_token& stop) { monitor_loop(stop); });
}

BackupPublisherManager::~BackupPublisherManager() {
    monitor_thread_.request_stop();
    // jthread's destructor joins; workers are destroyed (and joined) when
    // workers_ is cleared by this object's own destruction.
}

std::string BackupPublisherManager::key_of(std::string_view application, std::string_view stream) {
    return std::string(application) + "/" + std::string(stream);
}

void BackupPublisherManager::load_from_store() {
    if (store_ == nullptr) return;
    auto rows = store_->load_backup_publishers();
    if (!rows) {
        RTMP_LOG(observability::LogLevel::Warn, "backup-publisher", "load failed",
                 {{"error", rows.error().message()}});
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& row : rows.value()) {
        if (!protocol::parse_rtmp_url(row.backup_url)) {
            RTMP_LOG(observability::LogLevel::Warn, "backup-publisher", "row skipped",
                     {{"application", row.application},
                      {"stream", row.stream},
                      {"error", "stored URL is not a valid rtmp:// source"}});
            continue;
        }
        BackupPublisherConfig config;
        config.application = row.application;
        config.stream = row.stream;
        config.backup_url = row.backup_url;
        config.enabled = row.enabled;
        config.failover_after_seconds = row.failover_after_seconds;
        configs_[key_of(row.application, row.stream)] = std::move(config);
    }
}

core::Result<BackupPublisherStatus> BackupPublisherManager::upsert(const BackupPublisherConfig& config) {
    if (config.application.empty() || config.stream.empty()) {
        return config_error("application and stream are required");
    }
    auto parsed = protocol::parse_rtmp_url(config.backup_url);
    if (!parsed) return parsed.error();

    BackupPublisherConfig stored = config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = key_of(config.application, config.stream);
        configs_[key] = stored;
        const auto worker = workers_.find(key);
        if (worker != workers_.end()) worker->second->update_config(stored);
    }

    if (store_ != nullptr) {
        persistence::BackupPublisherRow row;
        row.application = stored.application;
        row.stream = stored.stream;
        row.backup_url = stored.backup_url;
        row.enabled = stored.enabled;
        row.failover_after_seconds = stored.failover_after_seconds;
        if (auto saved = store_->upsert_backup_publisher(row); !saved) return saved.error();
    }

    BackupPublisherStatus status;
    status.application = stored.application;
    status.stream = stored.stream;
    status.url_redacted = redact_rtmp_url(stored.backup_url);
    status.enabled = stored.enabled;
    status.state = BackupPublisherState::Standby;
    status.detail = "standby: primary is live";
    return status;
}

core::Result<void> BackupPublisherManager::remove(std::string_view application,
                                                  std::string_view stream) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = key_of(application, stream);
        if (configs_.erase(key) == 0) {
            return not_found_error("no such backup publisher");
        }
        workers_.erase(key); // destructor stops and joins the worker
    }
    if (store_ != nullptr) {
        if (auto deleted = store_->delete_backup_publisher(application, stream); !deleted) {
            return deleted.error();
        }
    }
    return {};
}

std::vector<BackupPublisherStatus> BackupPublisherManager::list(std::string_view application) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BackupPublisherStatus> result;
    for (const auto& [key, config] : configs_) {
        if (!application.empty() && config.application != application) continue;
        const auto worker = workers_.find(key);
        if (worker != workers_.end()) {
            result.push_back(worker->second->status());
        } else {
            BackupPublisherStatus status;
            status.application = config.application;
            status.stream = config.stream;
            status.url_redacted = redact_rtmp_url(config.backup_url);
            status.enabled = config.enabled;
            status.state = config.enabled ? BackupPublisherState::Standby
                                          : BackupPublisherState::Disabled;
            status.detail = config.enabled ? "standby: primary is live" : "disabled";
            result.push_back(std::move(status));
        }
    }
    std::ranges::sort(result, [](const BackupPublisherStatus& a, const BackupPublisherStatus& b) {
        return std::tie(a.application, a.stream) < std::tie(b.application, b.stream);
    });
    return result;
}

void BackupPublisherManager::monitor_loop(const std::stop_token& stop) {
    // application/stream -> steady-clock instant the primary was last observed
    // live. Absent means "never seen live since this manager started", which
    // is treated as down from the start so a backup takes over immediately
    // rather than waiting out a grace period for a publisher that was already
    // absent before this process came up.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_live;

    while (!stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::pair<std::string, BackupPublisherConfig>> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [key, config] : configs_) snapshot.emplace_back(key, config);
        }

        for (const auto& [key, config] : snapshot) {
            if (!config.enabled) continue;
            const bool live = is_live_ && is_live_(config.application, config.stream);
            if (live) {
                last_live[key] = now;
            }
            const auto since = last_live.find(key);
            // Strict '>', not '>=': a grace period of 0 must still recognise the
            // instant the primary is observed live again (elapsed 0 > 0 is
            // false) while still failing over immediately the first time a
            // stream is seen down (the "never observed live" branch below,
            // which does not depend on elapsed time at all). '>=' would make
            // a 0 s grace period permanently "down" -- 0 elapsed >= 0
            // configured is always true -- so recovery could never register.
            const bool primary_down =
                since == last_live.end() ||
                now - since->second > std::chrono::seconds(config.failover_after_seconds);

            std::shared_ptr<Worker> worker;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // The config may have been removed between the snapshot above
                // and this pass; skip it rather than resurrect a worker for a
                // deleted configuration.
                if (!configs_.contains(key)) continue;
                auto existing = workers_.find(key);
                if (existing == workers_.end()) {
                    if (!primary_down) continue; // nothing to do while healthy
                    existing = workers_.emplace(key, std::make_shared<Worker>(config, make_sink_)).first;
                }
                worker = existing->second;
            }
            worker->set_active(primary_down);
        }

        for (int i = 0; i < 20 && !stop.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace rtmp_server::relay
