#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "rtmp_server/control/http_server.hpp"
#include "rtmp_server/management/stream_manager.hpp"
#include "rtmp_server/observability/audit_log.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/persistence/store.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"

namespace rtmp_server::control {

struct ManagementApiOptions {
    // Bearer token required on every request (Authorization: Bearer
    // <token>). Compared with core::constant_time_equals. Empty means "no
    // token configured" — the API refuses to start serving mutating routes
    // in that case (fails closed; see ManagementApi::handle()).
    std::string admin_token;
    std::size_t max_auth_failures_per_ip = 20;
    std::chrono::seconds auth_failure_window{60};
    std::size_t default_page_size = 50;
    std::size_t max_page_size = 200;
};

// Routes HTTP requests for the endpoint set in docs/v2_promot.md Phase 5
// "Management API" onto management::StreamManager, producing structured
// JSON responses with request IDs, audit logging, pagination, bearer-token
// auth with per-IP failure rate limiting, and no secret leakage (the raw
// stream key/token appear only in the create-stream and rotate-key
// responses, matching StreamManager's own "only two moments" contract).
//
// `registry`/`fanout` are optional, non-owning pointers to the live
// publisher/viewer state (status, viewers, disconnect-* routes need them).
// When unset, those routes return 503 rather than fabricating an answer —
// see docs/management-api.md "Known limitations" for why: this class has
// no way to reach the actual TCP connections/session objects on this
// platform build (same standing gap as StreamManager's own
// Publisher/ViewerDisconnectHandler doc comment).
class ManagementApi {
public:
    ManagementApi(management::StreamManager& manager, ManagementApiOptions options);

    void set_store(persistence::Store* store) { store_ = store; }
    void set_audit_log(observability::AuditLog* audit_log) { audit_log_ = audit_log; }
    void set_metrics(observability::Metrics* metrics) { metrics_ = metrics; }
    void set_registry(protocol::commands::StreamRegistry* registry) { registry_ = registry; }
    void set_fanout(protocol::commands::LiveFanout* fanout) { fanout_ = fanout; }
    void set_stream_id_registry(protocol::commands::StreamIdRegistry* stream_ids) { stream_ids_ = stream_ids; }

    // Suitable for HttpServer::set_handler directly.
    [[nodiscard]] HttpResponse handle(const HttpRequest& request);

private:
    [[nodiscard]] bool authorized_locked(const std::string& client_ip, const HttpRequest& request);
    [[nodiscard]] HttpResponse route(const HttpRequest& request, const std::string& request_id);

    [[nodiscard]] HttpResponse handle_health_live();
    [[nodiscard]] HttpResponse handle_health_ready();
    [[nodiscard]] HttpResponse handle_metrics();
    [[nodiscard]] HttpResponse handle_list_applications(const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_create_application(const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_list_streams(const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_create_stream(const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_get_stream(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_patch_stream(std::string_view application, std::string_view name,
                                                     const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_rotate_key(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_playback_token(std::string_view application, std::string_view name,
                                                        const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_status(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_viewers(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_disconnect_publisher(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_disconnect_viewers(std::string_view application, std::string_view name);

    void audit(std::string_view action, std::string_view application, std::string_view name, bool success);

    management::StreamManager& manager_;
    ManagementApiOptions options_;
    persistence::Store* store_ = nullptr;
    observability::AuditLog* audit_log_ = nullptr;
    observability::Metrics* metrics_ = nullptr;
    protocol::commands::StreamRegistry* registry_ = nullptr;
    protocol::commands::LiveFanout* fanout_ = nullptr;
    protocol::commands::StreamIdRegistry* stream_ids_ = nullptr;

    struct FailureWindow {
        std::size_t count = 0;
        std::chrono::steady_clock::time_point window_start;
    };
    std::mutex auth_mutex_;
    std::unordered_map<std::string, FailureWindow> auth_failures_per_ip_;
    std::atomic<std::uint64_t> next_request_id_{1};
};

} // namespace rtmp_server::control
