#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/media_handoff_queue.hpp"
#include "rtmp_server/persistence/store.hpp"
#include "rtmp_server/protocol/commands/recorder_sink.hpp"
#include "rtmp_server/relay/stream_target.hpp"

namespace rtmp_server::relay {

// Owns the configured outbound destinations and attaches them to a publish.
//
// This is the whole of "push this stream somewhere else": a target whose URL
// names another origin of the same deployment is a relay (ingest capacity
// beyond one box), and a target whose URL names an external ingest is a stream
// target (distribution). They differ only in the `relay` flag they are
// reported under.
// Tuning for StreamTargetManager. At namespace scope rather than nested,
// because the constructor below defaults it: a nested type's default member
// initializers are not usable in a default argument of its own enclosing class.
struct StreamTargetOptions {
    media::HandoffLimits queue_limits;
    std::uint32_t restart_delay_seconds = 5;
};

class StreamTargetManager {
public:
    using Options = StreamTargetOptions;

    StreamTargetManager(persistence::Store* store, Options options = {});
    ~StreamTargetManager();

    // Rebuilds targets persisted by a previous run. A row that no longer
    // validates is skipped rather than failing startup.
    void load_from_store();

    // Called from the publish path. Returns nullptr when this stream has no
    // enabled target, which is the common case; otherwise a sink that forwards
    // the publish to every enabled target of that stream.
    [[nodiscard]] std::shared_ptr<protocol::commands::RecorderSink> create_sink(
        std::string_view application, std::string_view stream);

    // Stops and forgets the sinks attached to a stream, without touching its
    // configured targets.
    void release(std::string_view application, std::string_view stream);

    [[nodiscard]] core::Result<StreamTargetStatus> upsert(const StreamTargetConfig& config);
    [[nodiscard]] core::Result<void> remove(std::string_view application, std::string_view stream,
                                            std::string_view name);
    [[nodiscard]] core::Result<StreamTargetStatus> set_enabled(std::string_view application,
                                                               std::string_view stream,
                                                               std::string_view name, bool enabled);
    // Live status where a publisher is running, configured state otherwise.
    // Empty `application` lists every target.
    [[nodiscard]] std::vector<StreamTargetStatus> list(std::string_view application) const;

    // Targets currently connected and publishing, for /metrics.
    [[nodiscard]] std::size_t active_target_count() const;

    // A stream may fan out to at most this many destinations. Each one is a
    // full outbound copy of the publish, so this bounds egress amplification
    // from a single ingest.
    static constexpr std::size_t kMaxTargetsPerStream = 8;

private:
    struct StreamState {
        // Sinks currently attached to a live publisher, keyed by target name.
        std::unordered_map<std::string, std::shared_ptr<StreamTargetSink>> sinks;
    };

    [[nodiscard]] static std::string key_of(std::string_view application, std::string_view stream);
    [[nodiscard]] StreamTargetStatus status_for_locked(const StreamTargetConfig& config) const;
    void stop_locked(const std::string& key, std::string_view name);

    persistence::Store* store_ = nullptr;
    Options options_;
    mutable std::mutex mutex_;
    // application/stream -> target name -> configuration.
    std::unordered_map<std::string, std::unordered_map<std::string, StreamTargetConfig>> targets_;
    std::unordered_map<std::string, StreamState> streams_;
};

} // namespace rtmp_server::relay
