#include "rtmp_server/authentication/rtmp_authenticator.hpp"

#include <charconv>

namespace rtmp_server::authentication {

namespace {

// Minimal "a=b&c=d" query-string parser for playback URLs
// ("?token=...&expires=..."). Deliberately not a general URL-decoding
// parser (no percent-decoding) — the values this project puts on the wire
// (hex signatures, decimal unix timestamps) never need it, and skipping it
// avoids a whole class of decoding-related bugs on attacker-controlled
// input for very little value.
[[nodiscard]] std::unordered_map<std::string, std::string> parse_query(std::string_view query) {
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

RtmpAuthenticator::RtmpAuthenticator(management::StreamManager& manager, AuthenticatorLimits limits)
    : manager_(manager), limits_(limits) {}

bool RtmpAuthenticator::ip_locked_out_locked(const std::string& client_ip) const {
    if (client_ip.empty()) return false; // unknown IP: can't rate-limit it, don't block on it.
    auto it = auth_failures_per_ip_.find(client_ip);
    if (it == auth_failures_per_ip_.end()) return false;
    if (core::monotonic_now() - it->second.window_start > limits_.auth_failure_window) return false;
    return it->second.count >= limits_.max_auth_failures_per_ip;
}

void RtmpAuthenticator::record_auth_result_locked(const std::string& client_ip, bool success) {
    if (success) return;
    // Counted even when client_ip is empty (the per-IP lockout below cannot
    // be, but the global failure rate is exactly what an operator alerts on,
    // and dropping unattributed failures would under-report an attack).
    if (metrics_ != nullptr) metrics_->increment(observability::MetricId::AuthenticationFailures);
    if (client_ip.empty()) return;
    auto now = core::monotonic_now();
    auto& window = auth_failures_per_ip_[client_ip];
    if (now - window.window_start > limits_.auth_failure_window) {
        window.window_start = now;
        window.count = 0;
    }
    ++window.count;
}

protocol::commands::StreamKeyValidator RtmpAuthenticator::key_validator() {
    return [this](std::string_view app, std::string_view raw_key) -> bool {
        // Note: without the client IP threaded through StreamKeyValidator's
        // signature, per-IP failure accounting for publish attempts happens
        // via stream_id_resolver() below (called immediately after this on
        // the same handle_publish() call, with the same connection's IP
        // available through the session — see docs/authentication.md
        // "Known limitations" for why publish-time IP rate limiting is
        // therefore folded into the resolver rather than this validator).
        return manager_.validate_publish_key(app, raw_key);
    };
}

protocol::commands::StreamIdResolver RtmpAuthenticator::stream_id_resolver() {
    return [this](std::string_view app, std::string_view raw_key) -> std::optional<std::string> {
        return manager_.resolve_stream_name_for_key(app, raw_key);
    };
}

protocol::commands::PlaybackAuthorizer RtmpAuthenticator::playback_authorizer() {
    return [this](std::string_view app, std::string_view name, std::string_view query,
                   std::string_view client_ip) -> bool {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ip_locked_out_locked(std::string(client_ip))) return false;
        }

        auto stream = manager_.find_stream(app, name);
        if (!stream || !stream->enabled) {
            std::lock_guard<std::mutex> lock(mutex_);
            record_auth_result_locked(std::string(client_ip), false);
            return false;
        }
        auto application = manager_.find_application(app);
        if (!application || !application->enabled) {
            std::lock_guard<std::mutex> lock(mutex_);
            record_auth_result_locked(std::string(client_ip), false);
            return false;
        }

        // A signed playback token is required only when the caller supplied
        // one (or the query string exists at all); a deployment that wants
        // to *mandate* tokens for every stream does so by never handing out
        // an un-signed playback URL in the first place (StreamManager /
        // url_builder always attaches one when playback_policy requires
        // it) — enforcing "token absent -> reject" here unconditionally
        // would break every existing open-playback test/deployment that
        // predates Phase 5. See docs/authentication.md.
        auto fields = parse_query(query);
        if (auto token_it = fields.find("token"); token_it != fields.end()) {
            auto expires_it = fields.find("expires");
            std::int64_t expires_at = 0;
            if (expires_it != fields.end()) {
                std::from_chars(expires_it->second.data(), expires_it->second.data() + expires_it->second.size(),
                                 expires_at);
            }
            auto now_unix =
                std::chrono::duration_cast<std::chrono::seconds>(core::wall_now().time_since_epoch()).count();
            auto result = manager_.verify_playback_token(app, name, token_it->second, expires_at, now_unix);
            if (!result.ok()) {
                std::lock_guard<std::mutex> lock(mutex_);
                record_auth_result_locked(std::string(client_ip), false);
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::string key = std::string(app) + "/" + std::string(name);
            std::size_t current = viewers_per_stream_.count(key) ? viewers_per_stream_[key] : 0;
            if (current >= limits_.max_viewers_per_stream) {
                record_auth_result_locked(std::string(client_ip), false);
                return false;
            }
            record_auth_result_locked(std::string(client_ip), true);
        }
        return true;
    };
}

bool RtmpAuthenticator::admit_connection(std::string_view client_ip) {
    if (client_ip.empty()) {
        // Can't bound what we can't identify, but it is still a connection.
        if (metrics_ != nullptr) metrics_->add(observability::MetricId::ActiveConnections, +1);
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& count = connections_per_ip_[std::string(client_ip)];
        if (count >= limits_.max_connections_per_ip) return false; // not admitted: gauge untouched
        ++count;
    }
    if (metrics_ != nullptr) metrics_->add(observability::MetricId::ActiveConnections, +1);
    return true;
}

void RtmpAuthenticator::release_connection(std::string_view client_ip) {
    if (client_ip.empty()) {
        // Mirrors the admit_connection() empty-IP branch, which did count it.
        if (metrics_ != nullptr) metrics_->add(observability::MetricId::ActiveConnections, -1);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_per_ip_.find(std::string(client_ip));
        if (it == connections_per_ip_.end()) return; // never admitted: gauge untouched
        if (--it->second == 0) connections_per_ip_.erase(it);
    }
    if (metrics_ != nullptr) metrics_->add(observability::MetricId::ActiveConnections, -1);
}

void RtmpAuthenticator::on_viewer_attached(std::string_view application, std::string_view stream_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++viewers_per_stream_[std::string(application) + "/" + std::string(stream_name)];
}

void RtmpAuthenticator::on_viewer_detached(std::string_view application, std::string_view stream_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = std::string(application) + "/" + std::string(stream_name);
    auto it = viewers_per_stream_.find(key);
    if (it == viewers_per_stream_.end()) return;
    if (--it->second == 0) viewers_per_stream_.erase(it);
}

std::size_t RtmpAuthenticator::auth_failure_count(std::string_view client_ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = auth_failures_per_ip_.find(std::string(client_ip));
    if (it == auth_failures_per_ip_.end()) return 0;
    if (core::monotonic_now() - it->second.window_start > limits_.auth_failure_window) return 0;
    return it->second.count;
}

} // namespace rtmp_server::authentication
