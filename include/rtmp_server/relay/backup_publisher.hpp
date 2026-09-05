#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/persistence/store.hpp"
#include "rtmp_server/protocol/commands/recorder_sink.hpp"

namespace rtmp_server::relay {

// A designated fallback ingest point for one local stream: when the primary
// publisher is not live for `failover_after_seconds`, this stream's backup
// RTMP source is played and fed into the exact same packaging path (HLS,
// DASH, the transcode ladder, other targets) a real publish would use — so
// viewers see an uninterrupted stream sourced from wherever the backup feed
// is (a redundant encoder, a secondary contribution link, another origin).
//
// This is Wowza's "backup ingest point" / stream failover, done without a
// second physical publisher connection: the backup is pulled, not pushed.
struct BackupPublisherConfig {
    std::string application;
    std::string stream;
    std::string backup_url; // rtmp://...
    bool enabled = true;
    // How long the primary may be absent before the backup takes over. Short
    // enough that a real outage is covered quickly, long enough that a
    // publisher's routine reconnect (a few seconds, common on mobile
    // encoders) never triggers a needless failover.
    std::uint32_t failover_after_seconds = 15;
    std::uint32_t restart_delay_seconds = 5;
};

enum class BackupPublisherState {
    // Primary is live (or assumed live because nothing has been observed
    // yet); the backup is not being pulled.
    Standby,
    Connecting,
    // The backup is live and feeding the packaging path.
    Active,
    Error,
    Disabled,
};

struct BackupPublisherStatus {
    std::string application;
    std::string stream;
    std::string url_redacted;
    bool enabled = true;
    BackupPublisherState state = BackupPublisherState::Standby;
    std::string detail;
    std::uint64_t activations = 0;
    std::uint64_t bytes_in = 0;
};

// Monitors every configured stream's liveness and activates its backup source
// on failover.
//
// `IsPublisherLive` and `SinkFactory` are the only two things this needs from
// the composition root: whether a stream currently has a real publisher
// (StreamRegistry::snapshot()), and how to build the same packaging sink a
// real publish gets (the existing recorder_factory) — reusing it means a
// failover produces the identical HLS/DASH/transcode/target fan-out a live
// publisher would, with no separate code path to keep in sync.
class BackupPublisherManager {
public:
    using IsPublisherLive =
        std::function<bool(std::string_view application, std::string_view stream)>;
    using SinkFactory = std::function<std::shared_ptr<protocol::commands::RecorderSink>(
        std::string_view application, std::string_view stream)>;

    BackupPublisherManager(persistence::Store* store, IsPublisherLive is_live, SinkFactory make_sink);
    ~BackupPublisherManager();
    BackupPublisherManager(const BackupPublisherManager&) = delete;
    BackupPublisherManager& operator=(const BackupPublisherManager&) = delete;

    void load_from_store();

    [[nodiscard]] core::Result<BackupPublisherStatus> upsert(const BackupPublisherConfig& config);
    [[nodiscard]] core::Result<void> remove(std::string_view application, std::string_view stream);
    [[nodiscard]] std::vector<BackupPublisherStatus> list(std::string_view application) const;

private:
    class Worker;

    [[nodiscard]] static std::string key_of(std::string_view application, std::string_view stream);
    void monitor_loop(const std::stop_token& stop);

    persistence::Store* store_ = nullptr;
    IsPublisherLive is_live_;
    SinkFactory make_sink_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, BackupPublisherConfig> configs_;
    std::unordered_map<std::string, std::shared_ptr<Worker>> workers_;

    std::jthread monitor_thread_;
};

} // namespace rtmp_server::relay
