#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
    // Open by default: every request is served without a bearer-token check.
    // Embedders can still opt in when they need an authenticated boundary.
    bool require_authentication = false;
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
    using LiveStateProvider = std::function<std::vector<management::LiveState>()>;
    using StreamDeletedHandler = std::function<void(std::string_view application, std::string_view name)>;
    using TranscodingStatusProvider = std::function<std::string()>;
    using TranscodingAssignmentsProvider =
        std::function<core::Result<std::string>(std::string_view application)>;
    using TranscodingAssignmentUpdater =
        std::function<core::Result<std::string>(std::string_view application,
                                                std::string_view source_stream,
                                                std::string_view template_name,
                                                std::string_view rules)>;
    using TranscodingAssignmentRemover =
        std::function<core::Result<void>(std::string_view application,
                                         std::string_view source_stream)>;
    // Source-transcode jobs: pull an external URL, transcode per template, and
    // re-serve as HLS. The updater receives the parsed inputs and returns the
    // job JSON; the provider/remover mirror the assignment handlers.
    using SourceJobsProvider =
        std::function<core::Result<std::string>(std::string_view application)>;
    using SourceJobCreator = std::function<core::Result<std::string>(
        std::string_view application, std::string_view name, std::string_view source_url,
        std::string_view template_name, std::string_view rules, bool auto_restart,
        std::uint32_t restart_delay_seconds)>;
    using SourceJobRemover =
        std::function<core::Result<void>(std::string_view application, std::string_view name)>;
    using SourceJobEnabledSetter = std::function<core::Result<std::string>(
        std::string_view application, std::string_view name, bool enabled)>;
    using SourceJobRestarter = std::function<core::Result<std::string>(
        std::string_view application, std::string_view name)>;

    ManagementApi(management::StreamManager& manager, ManagementApiOptions options);

    void set_store(persistence::Store* store) { store_ = store; }
    void set_audit_log(observability::AuditLog* audit_log) { audit_log_ = audit_log; }
    void set_metrics(observability::Metrics* metrics) { metrics_ = metrics; }
    void set_registry(protocol::commands::StreamRegistry* registry) { registry_ = registry; }
    void set_fanout(protocol::commands::LiveFanout* fanout) { fanout_ = fanout; }
    void set_stream_id_registry(protocol::commands::StreamIdRegistry* stream_ids) { stream_ids_ = stream_ids; }
    void set_live_state_provider(LiveStateProvider provider) { live_state_provider_ = std::move(provider); }
    // Invoked at the start of every /metrics scrape, before the exposition is
    // rendered. Lets the composition root publish gauges it samples from
    // outside this layer -- notably the cache edge's viewer/egress totals,
    // which no component below the composition root can see. Must be cheap
    // and non-blocking: it runs on the management HTTP thread.
    void set_metrics_refresher(std::function<void()> refresher) {
        metrics_refresher_ = std::move(refresher);
    }
    void set_stream_deleted_handler(StreamDeletedHandler handler) {
        stream_deleted_handler_ = std::move(handler);
    }
    void set_transcoding_status_provider(TranscodingStatusProvider provider) {
        transcoding_status_provider_ = std::move(provider);
    }
    void set_transcoding_assignment_handlers(TranscodingAssignmentsProvider provider,
                                              TranscodingAssignmentUpdater updater,
                                              TranscodingAssignmentRemover remover) {
        transcoding_assignments_provider_ = std::move(provider);
        transcoding_assignment_updater_ = std::move(updater);
        transcoding_assignment_remover_ = std::move(remover);
    }
    void set_source_job_handlers(SourceJobsProvider provider, SourceJobCreator creator,
                                 SourceJobRemover remover, SourceJobEnabledSetter enabled_setter = {},
                                 SourceJobRestarter restarter = {}) {
        source_jobs_provider_ = std::move(provider);
        source_job_creator_ = std::move(creator);
        source_job_remover_ = std::move(remover);
        source_job_enabled_setter_ = std::move(enabled_setter);
        source_job_restarter_ = std::move(restarter);
    }

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
    [[nodiscard]] HttpResponse handle_delete_application(std::string_view name);
    [[nodiscard]] HttpResponse handle_list_streams(const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_create_stream(const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_get_stream(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_patch_stream(std::string_view application, std::string_view name,
                                                     const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_delete_stream(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_status(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_viewers(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_disconnect_publisher(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_disconnect_viewers(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_transcoding_status();
    [[nodiscard]] HttpResponse handle_list_source_jobs(const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_create_source_job(const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_delete_source_job(std::string_view application,
                                                        std::string_view name);
    [[nodiscard]] HttpResponse handle_restart_source_job(std::string_view application, std::string_view name);
    [[nodiscard]] HttpResponse handle_patch_source_job(std::string_view application, std::string_view name,
                                                       const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_list_transcoding_assignments(const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_put_transcoding_assignment(std::string_view application,
                                                                  std::string_view source_stream,
                                                                  const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_delete_transcoding_assignment(std::string_view application,
                                                                     std::string_view source_stream);
    [[nodiscard]] HttpResponse handle_list_templates();
    [[nodiscard]] HttpResponse handle_put_template(std::string_view id, const HttpRequest& request);
    [[nodiscard]] HttpResponse handle_delete_template(std::string_view id);

    void audit(std::string_view action, std::string_view application, std::string_view name, bool success);

    management::StreamManager& manager_;
    ManagementApiOptions options_;
    persistence::Store* store_ = nullptr;
    observability::AuditLog* audit_log_ = nullptr;
    observability::Metrics* metrics_ = nullptr;
    protocol::commands::StreamRegistry* registry_ = nullptr;
    protocol::commands::LiveFanout* fanout_ = nullptr;
    protocol::commands::StreamIdRegistry* stream_ids_ = nullptr;
    LiveStateProvider live_state_provider_;
    std::function<void()> metrics_refresher_;
    StreamDeletedHandler stream_deleted_handler_;
    TranscodingStatusProvider transcoding_status_provider_;
    TranscodingAssignmentsProvider transcoding_assignments_provider_;
    TranscodingAssignmentUpdater transcoding_assignment_updater_;
    TranscodingAssignmentRemover transcoding_assignment_remover_;
    SourceJobsProvider source_jobs_provider_;
    SourceJobCreator source_job_creator_;
    SourceJobRemover source_job_remover_;
    SourceJobEnabledSetter source_job_enabled_setter_;
    SourceJobRestarter source_job_restarter_;

    struct FailureWindow {
        std::size_t count = 0;
        std::chrono::steady_clock::time_point window_start;
    };
    std::mutex auth_mutex_;
    std::unordered_map<std::string, FailureWindow> auth_failures_per_ip_;
    std::atomic<std::uint64_t> next_request_id_{1};
};

} // namespace rtmp_server::control
