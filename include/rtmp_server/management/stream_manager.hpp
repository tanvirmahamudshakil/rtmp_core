#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rtmp_server/core/clock.hpp"
#include "rtmp_server/core/result.hpp"
#include "rtmp_server/observability/audit_log.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/persistence/store.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"

namespace rtmp_server::management {

struct Application {
    std::string name;
    bool enabled = true;
};

// Persistent (well — in-memory for Phase 8; Phase 9 adds real persistence,
// see docs/phase8-checklist.md "Known limitations") metadata for one stream.
// Deliberately holds no raw secret: the publish key is stored only as a
// SHA-256 hash (see core/hmac.hpp), matching docs/rtmp_promot.md "hashed key
// persistence where practical" and "do not allow key enumeration".
struct Stream {
    std::string application;
    std::string name; // public identifier, used in playback URLs
    bool enabled = true;
    bool recording_enabled = false;
    core::WallClock::time_point created_at;
    // Public RTMP playback URL (rtmp://host:port/app/name), no token. Filled
    // in by StreamManager when a Stream is handed back to the API so the panel
    // can show and copy the link for VLC/ffmpeg at any time, not only at
    // creation. Empty inside the in-memory record; populated on read.
    std::string playback_url;
};

// Returned only at creation and at key rotation — the one and only two
// moments the raw key is ever available outside this class (docs/
// rtmp_promot.md "Stream Creation Response" — "Only return the raw stream
// key: during creation, during explicit key rotation").
struct StreamCreationResult {
    Stream stream;
    std::string stream_key;
    std::string publish_url;
    std::string playback_url;
};

struct LiveState {
    std::string application;
    std::string name;
    bool is_live = false;
    std::size_t viewer_count = 0;
    // Cumulative egress bytes delivered to viewers of this stream. The admin
    // panel polls this repeatedly and derives a per-link bitrate from the
    // delta over its own poll interval, the same way it already derives
    // trend history from repeated snapshots.
    std::uint64_t egress_bytes_total = 0;
};

// Fired with the connection_id of the publisher/viewer session a management
// operation wants terminated. Not owned/wired to a real transport here — no
// socket-owning connection registry is reachable from this layer on this
// host (same standing gap every phase since Phase 4 has documented: no
// io_uring transport on macOS). Production wiring hooks these into whatever
// owns TcpConnection/ConnectionRegistry and actually closes the socket;
// here they are the extension point plus the "which connection(s)" lookup.
using PublisherDisconnectHandler = std::function<void(std::uint64_t connection_id)>;
// Viewer sessions aren't tracked by connection_id anywhere yet (LiveFanout's
// subscriber_id is a relay pointer, not a connection_id — see
// docs/rtmp-playback.md); the handler instead receives the raw stream_key so
// the (not-yet-built) session owner can find and close every CommandSession
// currently playing it.
using ViewerDisconnectHandler = std::function<void(std::string_view stream_key)>;

// Owns the Application/Stream domain model — creation, key
// generation/rotation, enable/disable, recording flags, signed playback
// tokens, disconnect controls, and live-state snapshots (Phase 8, docs/
// rtmp_promot.md "Management API and Link Generation"). Deliberately kept in
// the same "pure logic, no transport" shape as StreamRegistry/LiveFanout
// (Phase 4/7): mutex-guarded in-memory maps, no HTTP server, no database —
// see docs/control-api.md and docs/phase8-checklist.md "Known limitations"
// for what a real HTTP-facing management API would add on top of this.
class StreamManager {
public:
    struct Options {
        std::string public_hostname = "localhost";
        std::uint16_t rtmp_port = 1935;
        std::string token_signing_secret;
    };

    explicit StreamManager(Options options);

    core::Result<void> create_application(std::string name);
    core::Result<void> delete_application(std::string_view name);
    [[nodiscard]] std::vector<Application> list_applications() const;
    [[nodiscard]] std::optional<Application> find_application(std::string_view name) const;

    // Fails with ErrorCode::NotFound if `application` was not created first,
    // ErrorCode::Conflict if `name` already exists within it.
    core::Result<StreamCreationResult> create_stream(std::string_view application, std::string name,
                                                      bool recording_enabled = false);
    // Generates a new random key, replacing (invalidating) the previous one.
    // Existing signed playback tokens are unaffected (they aren't derived
    // from the publish key — see token.hpp).
    core::Result<std::string> rotate_key(std::string_view application, std::string_view name);
    core::Result<void> set_enabled(std::string_view application, std::string_view name, bool enabled);
    core::Result<void> set_recording_enabled(std::string_view application, std::string_view name, bool enabled);
    core::Result<void> delete_stream(std::string_view application, std::string_view name);

    [[nodiscard]] std::optional<Stream> find_stream(std::string_view application, std::string_view name) const;
    [[nodiscard]] std::vector<Stream> list_streams(std::string_view application) const;

    // Publish-time gate: true iff `application`/`raw_key` resolve to a
    // stream that is both known and enabled. Shaped to drop directly into
    // protocol::commands::StreamKeyValidator
    // (`[mgr](app, key){ return mgr.validate_publish_key(app, key); }`).
    // Disabled streams and unknown keys are indistinguishable to the caller
    // by design (docs/rtmp_promot.md "do not allow key enumeration").
    [[nodiscard]] bool validate_publish_key(std::string_view application, std::string_view raw_key) const;

    // Phase 5 (authentication): resolves a raw publish key to the public
    // stream `name` it belongs to, iff `application`/`raw_key` are known and
    // enabled — the canonical identifier both the publish path (via this
    // resolver) and the playback path (the public name passed to `play`)
    // converge on, so a stream is fanned out under one identity regardless
    // of which credential a client used (docs/v2_promot.md Phase 5 item 4:
    // "publish key and public stream name... map to the same internal
    // stream ID"). Unknown/disabled key or application: std::nullopt —
    // indistinguishable from "wrong key" by design (no key enumeration).
    [[nodiscard]] std::optional<std::string> resolve_stream_name_for_key(std::string_view application,
                                                                          std::string_view raw_key) const;

    [[nodiscard]] std::string sign_playback_token(std::string_view application, std::string_view name,
                                                   std::int64_t expires_at_unix) const;
    [[nodiscard]] core::Result<void> verify_playback_token(std::string_view application, std::string_view name,
                                                            std::string_view token, std::int64_t expires_at_unix,
                                                            std::int64_t now_unix) const;

    void set_publisher_disconnect_handler(PublisherDisconnectHandler handler);
    void set_viewer_disconnect_handler(ViewerDisconnectHandler handler);

    // Injects a persistence backend (Phase 9). Optional and non-owning, same
    // pattern as every other collaborator here: without one, StreamManager
    // behaves exactly as it did in Phase 8 (in-memory only, lost on
    // restart). Once set, every mutating call (create/delete/rotate/
    // enable/disable) also write-throughs to the store; a store failure is
    // logged to the audit log (if set) as success=false but does NOT roll
    // back or fail the in-memory operation — same "storage failure must not
    // take down the hot path" posture recording::Recorder established for
    // disk errors in Phase 6. Call load_from_store() once after setting this
    // (typically at startup) to hydrate applications_/streams_ from it.
    void set_store(persistence::Store* store);
    core::Result<void> load_from_store();

    // Injects an audit log (Phase 9). Optional/non-owning. When set, every
    // mutating call appends one AuditEntry regardless of outcome.
    void set_audit_log(observability::AuditLog* audit_log);

    // Injects a metrics registry (Phase 9). Optional/non-owning. When set,
    // mutating calls increment a matching "management.<action>_total"
    // counter.
    void set_metrics(observability::Metrics* metrics);

    // Resolves the stream's current raw key against `registry`'s live
    // publisher snapshot (by hash match — the raw key is never stored at
    // rest) and, if found, invokes the disconnect handler with its
    // connection_id. Fails with ErrorCode::NotFound if the stream isn't
    // currently being published.
    core::Result<void> disconnect_publisher(std::string_view application, std::string_view name,
                                             const protocol::commands::StreamRegistry& registry);
    // Same resolution, but invokes the viewer handler with the raw
    // stream_key (see ViewerDisconnectHandler doc). Fails with
    // ErrorCode::NotFound under the same condition.
    core::Result<void> disconnect_viewers(std::string_view application, std::string_view name,
                                           const protocol::commands::StreamRegistry& registry);

    // One row per known stream: whether it's currently being published
    // (cross-referenced against `registry` the same way disconnect_* is)
    // and its live viewer count (cross-referenced against `fanout`, by the
    // StreamId `stream_ids` resolves the raw key to — LiveFanout is keyed by
    // StreamId, not the raw stream key string, per docs/v2_promot.md PHASE
    // 3). If `stream_ids` has never seen this (app, raw key) pair (e.g. no
    // publisher/viewer has resolved it yet), viewer_count is reported as 0.
    [[nodiscard]] std::vector<LiveState> live_state(const protocol::commands::StreamRegistry& registry,
                                                     const protocol::commands::LiveFanout& fanout,
                                                     const protocol::commands::StreamIdRegistry& stream_ids) const;

private:
    struct StreamRecord {
        Stream meta;
        std::string key_hash;
    };
    struct ApplicationRecord {
        bool enabled = true;
        std::map<std::string, StreamRecord> streams;
    };

    // Finds the raw key currently registered for (application, name) by
    // scanning `registry`'s live publishers and hash-matching against the
    // stored key_hash. O(live publisher count); fine at management-API
    // scale (not the hot RTMP media path).
    [[nodiscard]] std::optional<std::string> resolve_live_key_locked(
        const std::string& application, const StreamRecord& record,
        const protocol::commands::StreamRegistry& registry) const;

    // Best-effort audit + metrics hooks — never fail the caller if these are
    // unset (they're optional) or if the underlying store write fails.
    void audit_locked(std::string_view action, std::string_view application, std::string_view name, bool success,
                       std::string_view detail = {});
    void count_locked(std::string_view action);

    mutable std::mutex mutex_;
    std::map<std::string, ApplicationRecord> applications_;
    Options options_;
    PublisherDisconnectHandler publisher_disconnect_handler_;
    ViewerDisconnectHandler viewer_disconnect_handler_;
    persistence::Store* store_ = nullptr;
    observability::AuditLog* audit_log_ = nullptr;
    observability::Metrics* metrics_ = nullptr;
};

} // namespace rtmp_server::management
