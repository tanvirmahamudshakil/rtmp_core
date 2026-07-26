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
       << (stream.recording_enabled ? "true" : "false") << "}";
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
std::pair<std::string, std::string> split_stream_id(std::string_view id) {
    auto colon = id.find(':');
    if (colon == std::string_view::npos) return {std::string(id), ""};
    return {std::string(id.substr(0, colon)), std::string(id.substr(colon + 1))};
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

    if (!authorized_locked(request.client_ip, request)) {
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

    auto parts = split_path(request.path);
    // Expected shapes: ["v1","applications"], ["v1","streams"],
    // ["v1","streams", "<id>"], ["v1","streams","<id>","<action>"].
    if (parts.size() >= 2 && parts[0] == "v1" && parts[1] == "applications") {
        if (parts.size() == 2 && request.method == "GET") return handle_list_applications(request);
        if (parts.size() == 2 && request.method == "POST") return handle_create_application(request);
    }
    if (parts.size() >= 2 && parts[0] == "v1" && parts[1] == "streams") {
        if (parts.size() == 2 && request.method == "GET") return handle_list_streams(request);
        if (parts.size() == 2 && request.method == "POST") return handle_create_stream(request);
        if (parts.size() == 3) {
            auto [application, name] = split_stream_id(parts[2]);
            if (request.method == "GET") return handle_get_stream(application, name);
            if (request.method == "PATCH") return handle_patch_stream(application, name, request);
        }
        if (parts.size() == 4 && request.method == "POST") {
            auto [application, name] = split_stream_id(parts[2]);
            if (parts[3] == "rotate-publish-key") return handle_rotate_key(application, name);
            if (parts[3] == "playback-token") return handle_playback_token(application, name, request);
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
    std::ostringstream os;
    for (const auto& [name, value] : metrics_->counters_snapshot()) os << name << " " << value << "\n";
    for (const auto& [name, value] : metrics_->gauges_snapshot()) os << name << " " << value << "\n";
    HttpResponse r;
    r.status = 200;
    r.content_type = "text/plain; version=0.0.4";
    r.body = os.str();
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
    const auto& created = result.value();
    std::ostringstream os;
    os << R"({"stream":)" << stream_json(created.stream) << R"(,"stream_key":")" << json_escape(created.stream_key)
       << R"(","publish_url":")" << json_escape(created.publish_url) << R"(","playback_url":")"
       << json_escape(created.playback_url) << "\"}";
    return HttpResponse::json(201, os.str());
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
        auto result = manager_.set_enabled(application, name, it->second == "true");
        audit("set_enabled", application, name, result.ok());
        if (!result.ok())
            return HttpResponse::json(http_status_for(result.error().code()),
                                       error_body("request_failed", result.error().message(), ""));
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

HttpResponse ManagementApi::handle_rotate_key(std::string_view application, std::string_view name) {
    auto result = manager_.rotate_key(application, name);
    audit("rotate_key", application, name, result.ok());
    if (!result.ok()) {
        return HttpResponse::json(http_status_for(result.error().code()),
                                   error_body("request_failed", result.error().message(), ""));
    }
    std::ostringstream os;
    os << R"({"stream_key":")" << json_escape(result.value()) << "\"}";
    return HttpResponse::json(200, os.str());
}

HttpResponse ManagementApi::handle_playback_token(std::string_view application, std::string_view name,
                                                     const HttpRequest& request) {
    auto stream = manager_.find_stream(application, name);
    if (!stream) return HttpResponse::json(404, error_body("not_found", "no such stream", ""));

    auto fields = parse_flat_json(request.body);
    std::int64_t ttl_seconds = 3600;
    if (auto it = fields.find("ttl_seconds"); it != fields.end()) {
        std::from_chars(it->second.data(), it->second.data() + it->second.size(), ttl_seconds);
    }
    auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                   .count();
    std::int64_t expires_at = now + ttl_seconds;
    std::string token = manager_.sign_playback_token(application, name, expires_at);
    audit("sign_playback_token", application, name, true);

    std::ostringstream os;
    os << R"({"token":")" << json_escape(token) << R"(","expires_at":)" << expires_at << "}";
    return HttpResponse::json(200, os.str());
}

HttpResponse ManagementApi::handle_status(std::string_view application, std::string_view name) {
    if (registry_ == nullptr || fanout_ == nullptr || stream_ids_ == nullptr) {
        return HttpResponse::json(503, error_body("state_source_unavailable",
                                                    "publisher/viewer state source not wired", ""));
    }
    auto states = manager_.live_state(*registry_, *fanout_, *stream_ids_);
    for (const auto& state : states) {
        if (state.application == application && state.name == name) {
            std::ostringstream os;
            os << R"({"application":")" << json_escape(state.application) << R"(","name":")"
               << json_escape(state.name) << R"(","is_live":)" << (state.is_live ? "true" : "false")
               << R"(,"viewer_count":)" << state.viewer_count << "}";
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
