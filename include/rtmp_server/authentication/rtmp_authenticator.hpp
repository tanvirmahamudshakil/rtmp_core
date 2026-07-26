#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "rtmp_server/core/clock.hpp"
#include "rtmp_server/management/authorization_cache.hpp"
#include "rtmp_server/management/stream_manager.hpp"
#include "rtmp_server/protocol/commands/command_session.hpp"

namespace rtmp_server::authentication {

// Bounds enforced by RtmpAuthenticator (docs/v2_promot.md section 3.5 /
// Phase 5 "Enforce ... Maximum publishers, Maximum viewers, Maximum
// connections per IP" + "authentication failure rate limiting"). Every
// field here is a resource controlled by a remote client and must be
// bounded per that section's rule.
struct AuthenticatorLimits {
    // Per stream: RTMP conventionally allows exactly one live publisher per
    // stream identity (a second `publish` for the same key is already
    // rejected by StreamRegistry::register_publisher as "in use" — this
    // field exists for symmetry/documentation and future multi-ingest use,
    // default keeps today's single-publisher behaviour).
    std::size_t max_publishers_per_stream = 1;
    std::size_t max_viewers_per_stream = 5000;
    std::size_t max_connections_per_ip = 100;
    // Authentication-failure rate limiting: an IP that fails this many
    // publish/play authorizations within `auth_failure_window` is locked
    // out (further attempts fail closed) until the window rolls over.
    std::size_t max_auth_failures_per_ip = 20;
    std::chrono::seconds auth_failure_window{60};
};

// Ties management::StreamManager (persistence-backed application/stream
// domain, hashed key validation, signed playback tokens) to
// protocol::commands::CommandSession's Phase 5 authorization hooks
// (StreamKeyValidator, StreamIdResolver, PlaybackAuthorizer), while
// enforcing the remote-resource bounds required by docs/v2_promot.md
// section 3.5: per-stream publisher/viewer counts, per-IP connection count,
// and authentication-failure rate limiting.
//
// Every method here is in-memory only (StreamManager's own maps + this
// class's counters) — no blocking database call happens on this path, so
// RTMP command-session threads calling these callbacks never block on disk
// or network I/O (docs/v2_promot.md section 3.6 / Phase 5 item 12: "Never
// perform blocking database calls on media workers"). StreamManager's own
// AuthorizationCache (if the caller wires one in front of
// validate_publish_key) additionally bounds how often the in-memory hash
// comparison itself runs per (app, key) pair.
//
// Thread-safe: all mutable state is behind a single mutex. Called at
// connection-lifecycle rate (publish/play/disconnect), not per-media-packet
// rate, so a single mutex is not a scalability concern here (same posture
// StreamManager itself already takes).
class RtmpAuthenticator {
public:
    RtmpAuthenticator(management::StreamManager& manager, AuthenticatorLimits limits);

    // --- Callbacks to wire into protocol::commands::CommandSession ---

    [[nodiscard]] protocol::commands::StreamKeyValidator key_validator();
    [[nodiscard]] protocol::commands::StreamIdResolver stream_id_resolver();
    [[nodiscard]] protocol::commands::PlaybackAuthorizer playback_authorizer();

    // --- Connection-lifecycle bookkeeping (call from the session owner) ---

    // Returns false (and does not admit the connection) if `client_ip` is
    // already at max_connections_per_ip. Call once per accepted TCP
    // connection, before any RTMP command is processed.
    [[nodiscard]] bool admit_connection(std::string_view client_ip);
    // Call once per closed TCP connection that was previously admitted.
    void release_connection(std::string_view client_ip);

    void on_viewer_attached(std::string_view application, std::string_view stream_name);
    void on_viewer_detached(std::string_view application, std::string_view stream_name);

    [[nodiscard]] std::size_t auth_failure_count(std::string_view client_ip) const;

private:
    struct FailureWindow {
        std::size_t count = 0;
        core::MonotonicClock::time_point window_start;
    };

    void record_auth_result_locked(const std::string& client_ip, bool success);
    [[nodiscard]] bool ip_locked_out_locked(const std::string& client_ip) const;

    management::StreamManager& manager_;
    AuthenticatorLimits limits_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::size_t> connections_per_ip_;
    std::unordered_map<std::string, FailureWindow> auth_failures_per_ip_;
    // Keyed by "application/name".
    std::unordered_map<std::string, std::size_t> viewers_per_stream_;
};

} // namespace rtmp_server::authentication
