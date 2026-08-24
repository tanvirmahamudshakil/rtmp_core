#include "rtmp_server/control/management_api.hpp"

#include <charconv>
#include <sstream>

#include "rtmp_server/core/hmac.hpp"
#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::control {

namespace {

using rtmp_server::observability::LogLevel;

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Flat top-level "key":"value" / "key":true|false / "key":123 extraction —
// deliberately not a general JSON parser (no nesting, no arrays): every
// request body this API accepts is a flat object of a handful of scalar
// fields (name, application, enabled, recording_enabled, ttl_seconds).
std::unordered_map<std::string, std::string> parse_flat_json(std::string_view body) {
    std::unordered_map<std::string, std::string> out;
    std::size_t i = 0;
    auto skip_ws = [&] {
        while (i < body.size() && (body[i] == ' ' || body[i] == '\n' || body[i] == '\t' || body[i] == '\r' ||
                                    body[i] == '{' || body[i] == ',')) {
            ++i;
        }
    };
    while (i < body.size()) {
        skip_ws();
        if (i >= body.size() || body[i] == '}') break;
        if (body[i] != '"') break;
        ++i;
        std::size_t key_start = i;
        while (i < body.size() && body[i] != '"') ++i;
        std::string key(body.substr(key_start, i - key_start));
        if (i >= body.size()) break;
        ++i; // closing quote
        while (i < body.size() && (body[i] == ':' || body[i] == ' ')) ++i;
        if (i >= body.size()) break;
        std::string value;
        if (body[i] == '"') {
            ++i;
            std::size_t val_start = i;
            while (i < body.size() && body[i] != '"') {
                if (body[i] == '\\' && i + 1 < body.size()) ++i;
                ++i;
            }
            value = std::string(body.substr(val_start, i - val_start));
            if (i < body.size()) ++i;
        } else {
            std::size_t val_start = i;
            while (i < body.size() && body[i] != ',' && body[i] != '}' && body[i] != ' ' && body[i] != '\n') ++i;
            value = std::string(body.substr(val_start, i - val_start));
        }
        out.emplace(std::move(key), std::move(value));
    }
    return out;
}

std::string application_json(const management::Application& app) {
    std::ostringstream os;
    os << R"({"name":")" << json_escape(app.name) << R"(","enabled":)" << (app.enabled ? "true" : "false") << "}";
    return os.str();
}

std::string stream_json(const management::Stream& stream) {
    std::ostringstream os;
    os << R"({"application":")" << json_escape(stream.application) << R"(","name":")" << json_escape(stream.name)
       << R"(","enabled":)" << (stream.enabled ? "true" : "false") << R"(,"recording_enabled":)"
       << (stream.recording_enabled ? "true" : "false") << R"(,"rtmp_url":")" << json_escape(stream.playback_url)
       << R"(","hls_path":"/hls/)" << json_escape(stream.application) << "/" << json_escape(stream.name)
       << R"(/index.m3u8")" << "}";
    return os.str();
}

std::string error_body(std::string_view code, std::string_view message, std::string_view request_id) {
    std::ostringstream os;
    os << R"({"error":")" << json_escape(code) << R"(","message":")" << json_escape(message)
       << R"(","request_id":")" << json_escape(request_id) << "\"}";
    return os.str();
}

int http_status_for(core::ErrorCode code) {
    switch (code) {
        case core::ErrorCode::NotFound: return 404;
        case core::ErrorCode::Conflict: return 409;
        case core::ErrorCode::Unauthorized: return 401;
        case core::ErrorCode::ExpiredToken: return 401;
        case core::ErrorCode::InvalidConfiguration:
        case core::ErrorCode::MissingConfiguration: return 400;
        default: return 500;
    }
}

// Splits a "/v1/streams/{id}/..." path into the stream id and remaining
// suffix. The stream id itself is "<application>:<name>" (colon, not
// slash, so it fits in one path segment).
// Percent-decodes a URL path segment. The panel builds stream-id path
// segments with encodeURIComponent("<app>:<name>"), so the ':' arrives as
// "%3A" and, without decoding, split_stream_id would never find the
// separator — every per-stream route (status, PATCH, rotate, token,
// disconnect) would then look up the whole "app%3Aname" as an application
// and fail with "application not found". Invalid/truncated escapes are left
// as-is rather than dropped.
std::string percent_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hex(in[i + 1]);
            int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

std::pair<std::string, std::string> split_stream_id(std::string_view id_raw) {
    std::string id = percent_decode(id_raw);
    auto colon = id.find(':');
    if (colon == std::string::npos) return {id, ""};
    return {id.substr(0, colon), id.substr(colon + 1)};
}

std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < path.size()) {
        auto slash = path.find('/', start);
        if (slash == std::string_view::npos) slash = path.size();
        if (slash > start) parts.emplace_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    return parts;
}

std::unordered_map<std::string, std::string> parse_query_params(std::string_view query) {
    std::unordered_map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < query.size()) {
        auto amp = query.find('&', pos);
        std::string_view pair = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        if (auto eq = pair.find('='); eq != std::string_view::npos) {
            out.emplace(std::string(pair.substr(0, eq)), std::string(pair.substr(eq + 1)));
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return out;
}

} // namespace

ManagementApi::ManagementApi(management::StreamManager& manager, ManagementApiOptions options)
    : manager_(manager), options_(std::move(options)) {}

bool ManagementApi::authorized_locked(const std::string& client_ip, const HttpRequest& request) {
    if (options_.admin_token.empty()) return false; // fail closed: no token configured means no access.

    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        if (!client_ip.empty()) {
            auto it = auth_failures_per_ip_.find(client_ip);
            if (it != auth_failures_per_ip_.end()) {
                auto now = std::chrono::steady_clock::now();
                if (now - it->second.window_start <= options_.auth_failure_window &&
                    it->second.count >= options_.max_auth_failures_per_ip) {
                    return false;
                }
            }
        }
    }

    auto it = request.headers.find("authorization");
    std::string presented;
    if (it != request.headers.end()) {
        static constexpr std::string_view kPrefix = "Bearer ";
        if (it->second.rfind(kPrefix, 0) == 0) presented = it->second.substr(kPrefix.size());
    }
    bool ok = !presented.empty() && core::constant_time_equals(presented, options_.admin_token);

    std::lock_guard<std::mutex> lock(auth_mutex_);
    if (!client_ip.empty()) {
        auto now = std::chrono::steady_clock::now();
        auto& window = auth_failures_per_ip_[client_ip];
        if (now - window.window_start > options_.auth_failure_window) {
            window.window_start = now;
            window.count = 0;
        }
        if (!ok) ++window.count;
    }
    return ok;
}

HttpResponse ManagementApi::handle(const HttpRequest& request) {
    std::string request_id = "req-" + std::to_string(next_request_id_.fetch_add(1, std::memory_order_relaxed));

    // Health/liveness never requires auth — a load balancer/orchestrator
    // must be able to probe it without a credential.
    if (request.method == "GET" && request.path == "/health/live") {
        return handle_health_live();
    }
    if (request.method == "GET" && request.path == "/health/ready") {
        return handle_health_ready();
    }

    if (options_.require_authentication && !authorized_locked(request.client_ip, request)) {
        HttpResponse r;
        r.status = 401;
        r.body = error_body("unauthorized", "missing or invalid bearer token", request_id);
        r.headers["X-Request-Id"] = request_id;
        return r;
    }

    HttpResponse response = route(request, request_id);
    response.headers["X-Request-Id"] = request_id;
    RTMP_LOG(LogLevel::Info, "management_api", "request",
             {{"request_id", request_id},
              {"method", request.method},
              {"path", request.path},
              {"status", std::to_string(response.status)}});
    return response;
}

HttpResponse ManagementApi::route(const HttpRequest& request, const std::string& request_id) {
    if (request.method == "GET" && request.path == "/metrics") return handle_metrics();
    if (request.method == "GET" && request.path == "/v1/transcoding/status") {
        return handle_transcoding_status();
    }

    auto parts = split_path(request.path);
    if (parts.size() >= 3 && parts[0] == "v1" && parts[1] == "transcoding" &&
        parts[2] == "assignments") {
        if (parts.size() == 3 && request.method == "GET") {
            return handle_list_transcoding_assignments(request);
        }
        if (parts.size() == 4) {
            auto [application, source_stream] = split_stream_id(parts[3]);
            if (request.method == "PUT") {
                return handle_put_transcoding_assignment(application, source_stream, request);
            }
            if (request.method == "DELETE") {
                return handle_delete_transcoding_assignment(application, source_stream);
            }
        }
    }
    if (parts.size() >= 3 && parts[0] == "v1" && parts[1] == "transcoding" &&
        parts[2] == "source-jobs") {
        if (parts.size() == 3 && request.method == "GET") return handle_list_source_jobs(request);
        if (parts.size() == 3 && request.method == "POST") return handle_create_source_job(request);
        if (parts.size() == 4 && request.method == "DELETE") {
            auto [application, name] = split_stream_id(parts[3]);
            return handle_delete_source_job(application, name);
        }
        if (parts.size() == 4 && request.method == "PATCH") {
            auto [application, name] = split_stream_id(parts[3]);
            return handle_patch_source_job(application, name, request);
        }
        if (parts.size() == 5 && request.method == "POST" && parts[4] == "restart") {
            auto [application, name] = split_stream_id(parts[3]);
            return handle_restart_source_job(application, name);
        }
    }
    // Expected shapes: ["v1","applications"], ["v1","streams"],
    // ["v1","streams", "<id>"], ["v1","streams","<id>","<action>"].
    if (parts.size() >= 2 && parts[0] == "v1" && parts[1] == "applications") {
        if (parts.size() == 2 && request.method == "GET") return handle_list_applications(request);
        if (parts.size() == 2 && request.method == "POST") return handle_create_application(request);
        if (parts.size() == 3 && request.method == "DELETE") return handle_delete_application(parts[2]);
    }
    if (parts.size() >= 2 && parts[0] == "v1" && parts[1] == "templates") {
        if (parts.size() == 2 && request.method == "GET") return handle_list_templates();
        if (parts.size() == 3 && request.method == "PUT") return handle_put_template(parts[2], request);
        if (parts.size() == 3 && request.method == "DELETE") return handle_delete_template(parts[2]);
    }
    if (parts.size() >= 2 && parts[0] == "v1" && parts[1] == "streams") {
        if (parts.size() == 2 && request.method == "GET") return handle_list_streams(request);
        if (parts.size() == 2 && request.method == "POST") return handle_create_stream(request);
        if (parts.size() == 3) {
            auto [application, name] = split_stream_id(parts[2]);
            if (request.method == "GET") return handle_get_stream(application, name);
            if (request.method == "PATCH") return handle_patch_stream(application, name, request);
            if (request.method == "DELETE") return handle_delete_stream(application, name);
        }
        if (parts.size() == 4 && request.method == "POST") {
            auto [application, name] = split_stream_id(parts[2]);
            if (parts[3] == "disconnect-publisher") return handle_disconnect_publisher(application, name);
            if (parts[3] == "disconnect-viewers") return handle_disconnect_viewers(application, name);
        }
        if (parts.size() == 4 && request.method == "GET") {
            auto [application, name] = split_stream_id(parts[2]);
            if (parts[3] == "status") return handle_status(application, name);
            if (parts[3] == "viewers") return handle_viewers(application, name);
        }
    }

    HttpResponse r;
    r.status = 404;
    r.body = error_body("not_found", "no such route", request_id);
    return r;
}

HttpResponse ManagementApi::handle_transcoding_status() {
    if (!transcoding_status_provider_) {
        return HttpResponse::json(200, R"({"enabled":false,"capabilities":[],"jobs":[]})");
    }
    return HttpResponse::json(200, transcoding_status_provider_());
}

HttpResponse ManagementApi::handle_list_transcoding_assignments(const HttpRequest& request) {
    if (!transcoding_assignments_provider_) {
        return HttpResponse::json(503, R"({"error":"transcoding_unavailable"})");
    }
    const auto params = parse_query_params(request.query);
    const auto application = params.find("application");
    if (application == params.end() || application->second.empty()) {
        return HttpResponse::json(400, R"({"error":"application_required"})");
    }
    auto result = transcoding_assignments_provider_(percent_decode(application->second));
    if (!result) return HttpResponse::json(500, error_body("request_failed", result.error().message(), ""));
    return HttpResponse::json(200, std::move(result).value());
}

HttpResponse ManagementApi::handle_put_transcoding_assignment(std::string_view application,
                                                                std::string_view source_stream,
                                                                const HttpRequest& request) {
    if (!transcoding_assignment_updater_) {
        return HttpResponse::json(503, R"({"error":"transcoding_unavailable"})");
    }
    const auto header = request.headers.find("x-template-name");
    if (application.empty() || source_stream.empty() || header == request.headers.end() ||
        header->second.empty() || header->second.size() > 128 || request.body.empty()) {
        return HttpResponse::json(400, R"({"error":"invalid_transcoding_assignment"})");
    }
    auto result = transcoding_assignment_updater_(application, source_stream, header->second, request.body);
    if (!result) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(200, std::move(result).value());
}

HttpResponse ManagementApi::handle_delete_transcoding_assignment(std::string_view application,
                                                                   std::string_view source_stream) {
    if (!transcoding_assignment_remover_) {
        return HttpResponse::json(503, R"({"error":"transcoding_unavailable"})");
    }
    auto result = transcoding_assignment_remover_(application, source_stream);
    if (!result) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(200, R"({"deleted":true})");
}

namespace {
std::string template_json(const persistence::TemplateRow& row) {
    std::ostringstream os;
    // presets_json is stored (and was received) as an already-valid JSON
    // array — embedded verbatim rather than re-escaped as a string, same as
    // every other opaque-JSON-blob field this API round-trips untouched.
    os << R"({"id":")" << json_escape(row.id) << R"(","name":")" << json_escape(row.name)
       << R"(","presets":)" << row.presets_json << "}";
    return os.str();
}
} // namespace

HttpResponse ManagementApi::handle_list_templates() {
    if (store_ == nullptr) return HttpResponse::json(503, R"({"error":"store_unavailable"})");
    auto result = store_->load_templates();
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    std::ostringstream os;
    os << R"({"items":[)";
    const auto& rows = result.value();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) os << ",";
        os << template_json(rows[i]);
    }
    os << R"(],"total":)" << rows.size() << "}";
    return HttpResponse::json(200, os.str());
}

HttpResponse ManagementApi::handle_put_template(std::string_view id, const HttpRequest& request) {
    if (store_ == nullptr) return HttpResponse::json(503, R"({"error":"store_unavailable"})");
    const auto header = request.headers.find("x-template-name");
    // presets is a JSON array (possibly empty: "[]") of encoding presets —
    // opaque to this layer, same treatment as transcoding_assignments.rules.
    if (id.empty() || header == request.headers.end() || header->second.empty() ||
        header->second.size() > 128 || request.body.empty() || request.body.front() != '[') {
        return HttpResponse::json(400, R"({"error":"invalid_template"})");
    }
    persistence::TemplateRow row;
    row.id = std::string(id);
    row.name = header->second;
    row.presets_json = request.body;
    auto result = store_->upsert_template(row);
    audit("upsert_template", "", id, result.ok());
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(200, template_json(row));
}

HttpResponse ManagementApi::handle_delete_template(std::string_view id) {
    if (store_ == nullptr) return HttpResponse::json(503, R"({"error":"store_unavailable"})");
    auto result = store_->delete_template(id);
    audit("delete_template", "", id, result.ok());
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(200, R"({"deleted":true})");
}

HttpResponse ManagementApi::handle_list_source_jobs(const HttpRequest& request) {
    if (!source_jobs_provider_) return HttpResponse::json(503, R"({"error":"transcoding_unavailable"})");
    const auto params = parse_query_params(request.query);
    const auto application = params.find("application");
    if (application == params.end() || application->second.empty()) {
        return HttpResponse::json(400, R"({"error":"application_required"})");
    }
    auto result = source_jobs_provider_(percent_decode(application->second));
    if (!result) return HttpResponse::json(500, error_body("request_failed", result.error().message(), ""));
    return HttpResponse::json(200, std::move(result).value());
}

HttpResponse ManagementApi::handle_create_source_job(const HttpRequest& request) {
    if (!source_job_creator_) return HttpResponse::json(503, R"({"error":"transcoding_unavailable"})");
    const auto app = request.headers.find("x-application");
    const auto name = request.headers.find("x-output-name");
    const auto source = request.headers.find("x-source-url");
    const auto tmpl = request.headers.find("x-template-name");
    const auto end = request.headers.end();
    if (app == end || name == end || source == end || tmpl == end || app->second.empty() ||
        name->second.empty() || source->second.empty() || request.body.empty()) {
        return HttpResponse::json(400, R"({"error":"invalid_source_job"})");
    }
    // Auto-restart is opt-out (defaults on) so an existing client that omits
    // the header still gets a self-healing source job; the delay defaults to
    // 5s if unset or unparsable.
    bool auto_restart = true;
    if (const auto it = request.headers.find("x-auto-restart"); it != end) {
        auto_restart = it->second != "false" && it->second != "0";
    }
    std::uint32_t restart_delay_seconds = 5;
    if (const auto it = request.headers.find("x-restart-delay-seconds"); it != end && !it->second.empty()) {
        std::uint32_t parsed = 0;
        const auto* begin = it->second.data();
        const auto* value_end = begin + it->second.size();
        if (auto [ptr, ec] = std::from_chars(begin, value_end, parsed); ec == std::errc{} && parsed > 0) {
            restart_delay_seconds = parsed;
        }
    }
    auto result = source_job_creator_(app->second, name->second, source->second, tmpl->second,
                                      request.body, auto_restart, restart_delay_seconds);
    if (!result) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(200, std::move(result).value());
}

HttpResponse ManagementApi::handle_delete_source_job(std::string_view application,
                                                     std::string_view name) {
    if (!source_job_remover_) return HttpResponse::json(503, R"({"error":"transcoding_unavailable"})");
    auto result = source_job_remover_(application, name);
    if (!result) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(200, R"({"deleted":true})");
}

HttpResponse ManagementApi::handle_patch_source_job(std::string_view application, std::string_view name,
                                                    const HttpRequest& request) {
    if (!source_job_enabled_setter_) return HttpResponse::json(503, R"({"error":"transcoding_unavailable"})");
    auto fields = parse_flat_json(request.body);
    auto it = fields.find("enabled");
    if (it == fields.end()) {
        return HttpResponse::json(400, error_body("validation_error", "no recognized fields in body", ""));
    }
    const bool enable = it->second == "true";
    auto result = source_job_enabled_setter_(application, name, enable);
    audit(enable ? "enable_source_job" : "disable_source_job", application, name, result.ok());
    if (!result) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(200, std::move(result).value());
}

HttpResponse ManagementApi::handle_restart_source_job(std::string_view application, std::string_view name) {
    if (!source_job_restarter_) return HttpResponse::json(503, R"({"error":"transcoding_unavailable"})");
    auto result = source_job_restarter_(application, name);
    audit("restart_source_job", application, name, result.ok());
    if (!result) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(200, std::move(result).value());
}

HttpResponse ManagementApi::handle_health_live() { return HttpResponse::json(200, R"({"status":"live"})"); }

HttpResponse ManagementApi::handle_health_ready() {
    // Readiness must reflect required dependencies (docs/v2_promot.md
    // Phase 5 definition of done): if a persistence store is configured,
    // it must actually answer; if not configured, the server runs in
    // in-memory-only mode and is ready by definition.
    if (store_ != nullptr) {
        auto result = store_->load_applications();
        if (!result.ok()) {
            return HttpResponse::json(503, R"({"status":"not_ready","reason":"store_unavailable"})");
        }
    }
    return HttpResponse::json(200, R"({"status":"ready"})");
}

HttpResponse ManagementApi::handle_metrics() {
    if (metrics_ == nullptr) return HttpResponse::json(200, "");

    // Composition-root gauges (cache-edge viewer/egress totals) are sampled
    // here so a scrape and the exposition it renders describe the same
    // moment.
    if (metrics_refresher_) metrics_refresher_();

    // Phase 7: refresh the derived series at scrape time. Both calls are
    // cheap, bounded and non-blocking (an RSS query and two subtractions) —
    // this handler already runs on the management HTTP thread, never on an
    // RTMP event-loop worker, so sampling here does not violate the
    // "no work on network workers" rule.
    metrics_->refresh_process_metrics();
    metrics_->refresh_derived();

    HttpResponse r;
    r.status = 200;
    r.content_type = "text/plain; version=0.0.4";
    // Full exposition format (HELP/TYPE + declared catalog + bounded
    // per-worker series + legacy dynamic names) instead of the bare
    // "name value" lines this endpoint emitted before Phase 7.
    r.body = metrics_->render_prometheus();
    return r;
}

HttpResponse ManagementApi::handle_list_applications(const HttpRequest& request) {
    auto params = parse_query_params(request.query);
    std::size_t offset = 0, limit = options_.default_page_size;
    if (auto it = params.find("offset"); it != params.end())
        std::from_chars(it->second.data(), it->second.data() + it->second.size(), offset);
    if (auto it = params.find("limit"); it != params.end())
        std::from_chars(it->second.data(), it->second.data() + it->second.size(), limit);
    limit = std::min(limit, options_.max_page_size);

    auto all = manager_.list_applications();
    std::ostringstream os;
    os << R"({"items":[)";
    std::size_t emitted = 0;
    for (std::size_t i = offset; i < all.size() && emitted < limit; ++i, ++emitted) {
        if (emitted > 0) os << ",";
        os << application_json(all[i]);
    }
    os << R"(],"total":)" << all.size() << R"(,"offset":)" << offset << R"(,"limit":)" << limit << "}";
    return HttpResponse::json(200, os.str());
}

HttpResponse ManagementApi::handle_create_application(const HttpRequest& request) {
    auto fields = parse_flat_json(request.body);
    auto it = fields.find("name");
    if (it == fields.end() || it->second.empty()) {
        return HttpResponse::json(400, error_body("validation_error", "name is required", ""));
    }
    auto result = manager_.create_application(it->second);
    audit("create_application", it->second, "", result.ok());
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                   error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(201, application_json(management::Application{it->second, true}));
}

HttpResponse ManagementApi::handle_delete_application(std::string_view name) {
    if (!manager_.find_application(name)) {
        return HttpResponse::json(404, error_body("not_found", "no such application", ""));
    }

    // delete_application() refuses to remove an application that still has
    // streams, so every link under it must be torn down first — same
    // disable-then-disconnect-then-delete sequence handle_delete_stream uses,
    // just applied to each stream in the application.
    for (const auto& stream : manager_.list_streams(name)) {
        static_cast<void>(manager_.set_enabled(name, stream.name, false));
        if (registry_ != nullptr) {
            static_cast<void>(manager_.disconnect_viewers(name, stream.name, *registry_));
            static_cast<void>(manager_.disconnect_publisher(name, stream.name, *registry_));
        }
        auto deleted = manager_.delete_stream(name, stream.name);
        audit("delete_stream", name, stream.name, deleted.ok());
        if (!deleted.ok()) {
            return HttpResponse::json(http_status_for(deleted.error().code()),
                                      error_body("request_failed", deleted.error().message(), ""));
        }
        if (stream_deleted_handler_) stream_deleted_handler_(name, stream.name);
    }

    auto result = manager_.delete_application(name);
    audit("delete_application", name, "", result.ok());
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(200, R"({"deleted":true})");
}

HttpResponse ManagementApi::handle_list_streams(const HttpRequest& request) {
    auto params = parse_query_params(request.query);
    auto app_it = params.find("application");
    if (app_it == params.end()) {
        return HttpResponse::json(400, error_body("validation_error", "application query param is required", ""));
    }
    auto streams = manager_.list_streams(app_it->second);
    std::ostringstream os;
    os << R"({"items":[)";
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (i > 0) os << ",";
        os << stream_json(streams[i]);
    }
    os << R"(],"total":)" << streams.size() << "}";
    return HttpResponse::json(200, os.str());
}

HttpResponse ManagementApi::handle_create_stream(const HttpRequest& request) {
    auto fields = parse_flat_json(request.body);
    auto app_it = fields.find("application");
    auto name_it = fields.find("name");
    if (app_it == fields.end() || name_it == fields.end() || app_it->second.empty() || name_it->second.empty()) {
        return HttpResponse::json(400, error_body("validation_error", "application and name are required", ""));
    }
    bool recording = false;
    if (auto rec_it = fields.find("recording_enabled"); rec_it != fields.end()) recording = rec_it->second == "true";

    auto result = manager_.create_stream(app_it->second, name_it->second, recording);
    audit("create_stream", app_it->second, name_it->second, result.ok());
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                   error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(201, stream_json(result.value().stream));
}

HttpResponse ManagementApi::handle_get_stream(std::string_view application, std::string_view name) {
    auto stream = manager_.find_stream(application, name);
    if (!stream) return HttpResponse::json(404, error_body("not_found", "no such stream", ""));
    return HttpResponse::json(200, stream_json(*stream));
}

HttpResponse ManagementApi::handle_patch_stream(std::string_view application, std::string_view name,
                                                  const HttpRequest& request) {
    auto fields = parse_flat_json(request.body);
    bool any = false;
    if (auto it = fields.find("enabled"); it != fields.end()) {
        const bool enable = it->second == "true";
        auto result = manager_.set_enabled(application, name, enable);
        audit("set_enabled", application, name, result.ok());
        if (!result.ok())
            return HttpResponse::json(http_status_for(result.error().code()),
                                       error_body("request_failed", result.error().message(), ""));
        // Disabling must take effect now, not just for future connections: drop
        // the live publisher and viewers so the RTMP feed and its HLS window
        // stop immediately (the HLS handler already 404s a disabled stream).
        if (!enable && registry_ != nullptr) {
            static_cast<void>(manager_.disconnect_viewers(application, name, *registry_));
            static_cast<void>(manager_.disconnect_publisher(application, name, *registry_));
        }
        any = true;
    }
    if (auto it = fields.find("recording_enabled"); it != fields.end()) {
        auto result = manager_.set_recording_enabled(application, name, it->second == "true");
        audit("set_recording_enabled", application, name, result.ok());
        if (!result.ok())
            return HttpResponse::json(http_status_for(result.error().code()),
                                       error_body("request_failed", result.error().message(), ""));
        any = true;
    }
    if (!any) return HttpResponse::json(400, error_body("validation_error", "no recognized fields in body", ""));

    auto stream = manager_.find_stream(application, name);
    if (!stream) return HttpResponse::json(404, error_body("not_found", "no such stream", ""));
    return HttpResponse::json(200, stream_json(*stream));
}

HttpResponse ManagementApi::handle_delete_stream(std::string_view application, std::string_view name) {
    if (!manager_.find_stream(application, name)) {
        return HttpResponse::json(404, error_body("not_found", "no such stream", ""));
    }

    // Stop new sessions first. A live stream is then disconnected before its
    // persistent definition is removed; disconnecting an already-idle stream
    // naturally returns NotFound and is intentionally harmless here.
    auto disabled = manager_.set_enabled(application, name, false);
    if (!disabled.ok()) {
        return HttpResponse::json(http_status_for(disabled.error().code()),
                                  error_body("request_failed", disabled.error().message(), ""));
    }
    if (registry_ != nullptr) {
        static_cast<void>(manager_.disconnect_viewers(application, name, *registry_));
        static_cast<void>(manager_.disconnect_publisher(application, name, *registry_));
    }

    auto result = manager_.delete_stream(application, name);
    audit("delete_stream", application, name, result.ok());
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                  error_body("request_failed", result.error().message(), ""));
    }
    if (stream_deleted_handler_) stream_deleted_handler_(application, name);
    return HttpResponse::json(200, R"({"deleted":true})");
}

HttpResponse ManagementApi::handle_status(std::string_view application, std::string_view name) {
    std::vector<management::LiveState> states;
    if (live_state_provider_) {
        states = live_state_provider_();
    } else if (registry_ != nullptr && fanout_ != nullptr && stream_ids_ != nullptr) {
        states = manager_.live_state(*registry_, *fanout_, *stream_ids_);
    } else {
        return HttpResponse::json(503, error_body("state_source_unavailable",
                                                    "publisher/viewer state source not wired", ""));
    }
    for (const auto& state : states) {
        if (state.application == application && state.name == name) {
            std::ostringstream os;
            // viewer_count is the total across delivery protocols; the
            // breakdown is reported alongside it so a client can show where
            // an audience actually is, and hls_viewers_measured tells it
            // whether the HLS half is a cache-edge measurement or only what
            // this origin could see for itself.
            os << R"({"application":")" << json_escape(state.application) << R"(","name":")"
               << json_escape(state.name) << R"(","is_live":)" << (state.is_live ? "true" : "false")
               << R"(,"viewer_count":)" << state.viewer_count
               << R"(,"rtmp_viewer_count":)" << state.rtmp_viewer_count
               << R"(,"hls_viewer_count":)" << state.hls_viewer_count
               << R"(,"hls_viewers_measured":)" << (state.hls_viewers_measured ? "true" : "false")
               << R"(,"egress_bytes_total":)" << state.egress_bytes_total
               << R"(,"rtmp_egress_bytes_total":)" << state.rtmp_egress_bytes_total
               << R"(,"hls_egress_bytes_total":)" << state.hls_egress_bytes_total << "}";
            return HttpResponse::json(200, os.str());
        }
    }
    return HttpResponse::json(404, error_body("not_found", "no such stream", ""));
}

HttpResponse ManagementApi::handle_viewers(std::string_view application, std::string_view name) {
    return handle_status(application, name); // viewer_count is already part of the status payload.
}

HttpResponse ManagementApi::handle_disconnect_publisher(std::string_view application, std::string_view name) {
    if (registry_ == nullptr) {
        return HttpResponse::json(503, error_body("state_source_unavailable", "publisher registry not wired", ""));
    }
    auto result = manager_.disconnect_publisher(application, name, *registry_);
    audit("disconnect_publisher", application, name, result.ok());
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                   error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(202, R"({"status":"accepted"})");
}

HttpResponse ManagementApi::handle_disconnect_viewers(std::string_view application, std::string_view name) {
    if (registry_ == nullptr) {
        return HttpResponse::json(503, error_body("state_source_unavailable", "publisher registry not wired", ""));
    }
    auto result = manager_.disconnect_viewers(application, name, *registry_);
    audit("disconnect_viewers", application, name, result.ok());
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                   error_body("request_failed", result.error().message(), ""));
    }
    return HttpResponse::json(202, R"({"status":"accepted"})");
}

void ManagementApi::audit(std::string_view action, std::string_view application, std::string_view name,
                           bool success) {
    if (audit_log_ == nullptr) return;
    observability::AuditEntry entry;
    entry.timestamp = core::wall_now();
    entry.actor = "management-api";
    entry.action = std::string(action);
    entry.application = std::string(application);
    entry.stream_name = std::string(name);
    entry.success = success;
    audit_log_->record(std::move(entry));
    if (metrics_ != nullptr) metrics_->increment_counter("management." + std::string(action) + "_total");
}

} // namespace rtmp_server::control
