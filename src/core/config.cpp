#include "rtmp_server/core/config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace rtmp_server::core {

namespace {

// Minimal flat "key: value" line parser — intentionally not a general YAML
// parser. Our config (config/server.example.yaml) is a single flat mapping
// with no nesting, lists, or anchors, so this covers 100% of it while
// avoiding a third-party YAML dependency for Phase 1. Comments start with
// '#'; values may be quoted or bare; duration values keep their unit suffix
// (e.g. "5s") and are parsed by the caller field-by-field below.
std::unordered_map<std::string, std::string> parse_flat_mapping(std::istream& in) {
    std::unordered_map<std::string, std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        auto trim = [](std::string& s) {
            const char* ws = " \t\r\n\"";
            auto first = s.find_first_not_of(ws);
            if (first == std::string::npos) {
                s.clear();
                return;
            }
            auto last = s.find_last_not_of(ws);
            s = s.substr(first, last - first + 1);
        };
        trim(key);
        trim(value);
        if (key.empty()) continue;

        out.emplace(std::move(key), std::move(value));
    }
    return out;
}

[[nodiscard]] std::optional<std::uint32_t> parse_u32(const std::string& s) {
    try {
        return static_cast<std::uint32_t>(std::stoul(s));
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::uint64_t> parse_u64(const std::string& s) {
    try {
        return static_cast<std::uint64_t>(std::stoull(s));
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<bool> parse_bool(const std::string& s) {
    if (s == "true") return true;
    if (s == "false") return false;
    return std::nullopt;
}

// Parses durations of the form "<number><unit>" where unit is one of
// ms|s|m|h, matching the style used in config/server.example.yaml.
[[nodiscard]] std::optional<std::chrono::milliseconds> parse_duration(const std::string& s) {
    if (s.empty()) return std::nullopt;
    std::size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) != 0)) ++i;
    if (i == 0) return std::nullopt;

    std::uint64_t number = 0;
    try {
        number = std::stoull(s.substr(0, i));
    } catch (...) {
        return std::nullopt;
    }
    std::string unit = s.substr(i);

    if (unit == "ms") return std::chrono::milliseconds(number);
    if (unit == "s") return std::chrono::milliseconds(number * 1000);
    if (unit == "m") return std::chrono::milliseconds(number * 60'000);
    if (unit == "h") return std::chrono::milliseconds(number * 3'600'000);
    return std::nullopt;
}

void apply_env_overrides(std::unordered_map<std::string, std::string>& values) {
    // Any key present in the config may be overridden via
    // RTMP_SERVER_<UPPER_SNAKE_KEY>, e.g. RTMP_SERVER_RTMP_PORT=1936.
    for (auto& [key, value] : values) {
        std::string env_name = "RTMP_SERVER_";
        for (char c : key) env_name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (const char* env_value = std::getenv(env_name.c_str())) {
            value = env_value;
        }
    }
}

// Rejects secrets that are absent, too short to resist offline brute force,
// or one of the placeholder values that ship in example configuration and
// tutorials. Phase 8 release gate: "unsupported insecure defaults are used"
// must fail the deployment rather than produce a warning nobody reads.
//
// The placeholder list is a deny-list, which is normally the wrong shape --
// but here it is only a courtesy check layered on top of the length rule,
// which is what actually provides the security property. Its job is to turn
// "your secret is too short" into an unmistakable "you left the example value
// in place".
Result<void> validate_secret(const std::string& value, const char* name) {
    static constexpr std::string_view kPlaceholders[] = {
        "CHANGE_ME", "changeme", "change_me", "secret", "password", "test",
        "changethis", "REPLACE_ME", "xxxxxxxx", "0123456789", "TODO",
    };

    if (value.empty()) {
        return Error(ErrorCode::MissingConfiguration, ErrorCategory::Configuration,
                      std::string(name) + " must be set");
    }
    for (const auto placeholder : kPlaceholders) {
        if (value == placeholder) {
            return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                          std::string(name) + " is a well-known placeholder value; generate a real secret");
        }
    }
    // 32 bytes matches the HMAC-SHA256 block/output size the token scheme
    // uses: a shorter key adds no security over a 32-byte one and a much
    // shorter one is brute-forceable offline from a single observed token.
    if (value.size() < kMinSecretLength) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      std::string(name) + " must be at least " + std::to_string(kMinSecretLength) +
                          " characters (generate with: openssl rand -hex 32)");
    }
    // A secret made of one repeated character has far less entropy than its
    // length suggests; this catches "aaaa...".
    if (value.find_first_not_of(value.front()) == std::string::npos) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      std::string(name) + " has no entropy (all characters identical)");
    }
    return {};
}

} // namespace

Result<void> ServerConfig::validate() const {
    if (rtmp_port == 0 || api_port == 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "rtmp_port and api_port must be non-zero");
    }
    if (rtmp_port == api_port && rtmp_bind_address == api_bind_address) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "rtmp_port and api_port must differ when bind addresses match");
    }
    if (ring_queue_depth == 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "ring_queue_depth must be positive");
    }
    if (completion_batch_size == 0 || completion_batch_size > ring_queue_depth ||
        submission_batch_size == 0 || submission_batch_size > ring_queue_depth) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "completion/submission batch sizes must be positive and no larger than ring_queue_depth");
    }
    if (max_worker_ring_count == 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "max_worker_ring_count must be positive");
    }
    if (worker_ring_count > max_worker_ring_count) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "worker_ring_count must not exceed max_worker_ring_count");
    }
    if (maximum_connections == 0 || maximum_connections_per_ip == 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "connection limits must be positive");
    }
    if (input_chunk_size == 0 || output_chunk_size == 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "chunk sizes must be positive");
    }
    if (transcode_cpu_reservation_percent > 100) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "transcode_cpu_reservation_percent must be between 0 and 100");
    }
    // Client socket tuning. A pinned buffer below one TCP segment's worth is
    // always a misconfiguration, and one above 64 MiB per connection is a
    // memory-exhaustion footgun at any real fan-out. 0 (kernel autotune) is
    // always allowed. setsockopt itself would silently clamp instead of
    // erroring, so the check lives here where the message is legible.
    {
        constexpr std::uint32_t kMinPinnedSocketBuffer = 2048;
        constexpr std::uint32_t kMaxPinnedSocketBuffer = 64u * 1024u * 1024u;
        auto check_buffer = [](std::uint32_t value, const char* name) -> Result<void> {
            if (value != 0 && (value < kMinPinnedSocketBuffer || value > kMaxPinnedSocketBuffer)) {
                return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                              std::string(name) +
                                  " must be 0 (kernel autotune) or between 2048 and 67108864 bytes");
            }
            return {};
        };
        if (auto r = check_buffer(client_send_buffer_bytes, "client_send_buffer_bytes"); !r) return r;
        if (auto r = check_buffer(client_receive_buffer_bytes, "client_receive_buffer_bytes"); !r) return r;
        if (client_tcp_notsent_lowat_bytes != 0 && client_send_buffer_bytes != 0 &&
            client_tcp_notsent_lowat_bytes > client_send_buffer_bytes) {
            return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                          "client_tcp_notsent_lowat_bytes must not exceed client_send_buffer_bytes");
        }
    }
    // --- Phase 8 release gate: "required configuration is missing" and
    // "unsupported insecure defaults are used" must both fail startup, not
    // warn. Everything below is enforced at load_config() time, so a
    // misconfigured deployment cannot reach a listening state.

    if (maximum_rtmp_message_size == 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "maximum_rtmp_message_size must be positive");
    }
    // An unbounded message size defeats every downstream reassembly bound
    // (ChunkDecoderLimits sizes its per-connection budget from this value).
    if (maximum_rtmp_message_size > kMaxSupportedRtmpMessageSize) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "maximum_rtmp_message_size exceeds the supported maximum (64 MiB)");
    }
    if (maximum_publishers == 0 || maximum_viewers_per_stream == 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "maximum_publishers and maximum_viewers_per_stream must be positive");
    }
    // Every remote-controlled queue must have a real bound (docs/v2_promot.md
    // section 3.5). A zero here reads as "unlimited", which is precisely the
    // failure mode that section exists to prevent.
    if (gop_cache_max_bytes == 0 || gop_cache_max_packets == 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "GOP cache bounds must be positive");
    }
    if (subscriber_queue_max_bytes == 0 || subscriber_queue_max_packets == 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "subscriber queue bounds must be positive");
    }
    if (idle_timeout.count() <= 0 || handshake_timeout.count() <= 0) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "idle_timeout and handshake_timeout must be positive");
    }
    if (recording_enabled && recording_directory.empty()) {
        return Error(ErrorCode::MissingConfiguration, ErrorCategory::Configuration,
                      "recording_directory is required when recording_enabled is true");
    }
    if (database_connection.empty()) {
        return Error(ErrorCode::MissingConfiguration, ErrorCategory::Configuration,
                      "database_connection must be set");
    }
    if (auto r = validate_secret(token_signing_secret, "token_signing_secret"); !r) return r;
    if (auto r = validate_secret(api_authentication_secret, "api_authentication_secret"); !r) return r;

    // Distinct secrets: reusing one value means a leaked management-API
    // credential also forges playback tokens for every stream, and rotating
    // either forces rotation of both.
    if (token_signing_secret == api_authentication_secret) {
        return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                      "token_signing_secret and api_authentication_secret must be different values");
    }

    return {};
}

Result<ServerConfig> load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Error(ErrorCode::MissingConfiguration, ErrorCategory::Configuration,
                      "config file not found: " + path);
    }

    auto values = parse_flat_mapping(file);
    apply_env_overrides(values);

    ServerConfig cfg;
    auto str = [&](const char* key, std::string& field) {
        if (auto it = values.find(key); it != values.end()) field = it->second;
    };
    auto u32 = [&](const char* key, std::uint32_t& field) {
        if (auto it = values.find(key); it != values.end()) {
            if (auto v = parse_u32(it->second)) field = *v;
        }
    };
    auto u64 = [&](const char* key, std::uint64_t& field) {
        if (auto it = values.find(key); it != values.end()) {
            if (auto v = parse_u64(it->second)) field = *v;
        }
    };
    auto boolean = [&](const char* key, bool& field) {
        if (auto it = values.find(key); it != values.end()) {
            if (auto v = parse_bool(it->second)) field = *v;
        }
    };
    auto duration = [&](const char* key, std::chrono::milliseconds& field) {
        if (auto it = values.find(key); it != values.end()) {
            if (auto v = parse_duration(it->second)) field = *v;
        }
    };

    str("rtmp_bind_address", cfg.rtmp_bind_address);
    str("api_bind_address", cfg.api_bind_address);
    str("public_rtmp_hostname", cfg.public_rtmp_hostname);

    if (auto it = values.find("rtmp_port"); it != values.end()) {
        if (auto v = parse_u32(it->second)) cfg.rtmp_port = static_cast<std::uint16_t>(*v);
    }
    if (auto it = values.find("api_port"); it != values.end()) {
        if (auto v = parse_u32(it->second)) cfg.api_port = static_cast<std::uint16_t>(*v);
    }

    u32("ring_queue_depth", cfg.ring_queue_depth);
    u32("completion_batch_size", cfg.completion_batch_size);
    u32("submission_batch_size", cfg.submission_batch_size);
    u32("worker_ring_count", cfg.worker_ring_count);
    u32("max_worker_ring_count", cfg.max_worker_ring_count);
    boolean("worker_cpu_pinning_enabled", cfg.worker_cpu_pinning_enabled);
    u32("transcode_cpu_reservation_percent", cfg.transcode_cpu_reservation_percent);

    boolean("enable_multishot_accept", cfg.enable_multishot_accept);
    boolean("enable_multishot_recv", cfg.enable_multishot_recv);
    boolean("enable_registered_buffers", cfg.enable_registered_buffers);
    boolean("enable_provided_buffer_ring", cfg.enable_provided_buffer_ring);
    boolean("enable_send_zero_copy", cfg.enable_send_zero_copy);
    boolean("enable_sqpoll", cfg.enable_sqpoll);
    u32("sqpoll_idle_ms", cfg.sqpoll_idle_ms);

    u32("registered_buffer_count", cfg.registered_buffer_count);
    u32("registered_buffer_size", cfg.registered_buffer_size);
    u32("provided_buffer_count", cfg.provided_buffer_count);
    u32("provided_buffer_size", cfg.provided_buffer_size);

    u32("client_send_buffer_bytes", cfg.client_send_buffer_bytes);
    u32("client_receive_buffer_bytes", cfg.client_receive_buffer_bytes);
    u32("client_tcp_notsent_lowat_bytes", cfg.client_tcp_notsent_lowat_bytes);

    u32("maximum_connections", cfg.maximum_connections);
    u32("maximum_connections_per_ip", cfg.maximum_connections_per_ip);
    u32("maximum_publishers", cfg.maximum_publishers);
    u32("maximum_viewers_per_stream", cfg.maximum_viewers_per_stream);

    u32("input_chunk_size", cfg.input_chunk_size);
    u32("output_chunk_size", cfg.output_chunk_size);
    u32("maximum_rtmp_message_size", cfg.maximum_rtmp_message_size);
    boolean("enable_hls_fast_join", cfg.enable_hls_fast_join);

    duration("handshake_timeout", cfg.handshake_timeout);
    duration("authentication_timeout", cfg.authentication_timeout);
    duration("idle_timeout", cfg.idle_timeout);
    duration("write_timeout", cfg.write_timeout);
    duration("publisher_inactivity_timeout", cfg.publisher_inactivity_timeout);
    duration("gop_cache_max_duration", cfg.gop_cache_max_duration);

    u64("gop_cache_max_bytes", cfg.gop_cache_max_bytes);
    u32("gop_cache_max_packets", cfg.gop_cache_max_packets);
    u64("subscriber_queue_max_bytes", cfg.subscriber_queue_max_bytes);
    u32("subscriber_queue_max_packets", cfg.subscriber_queue_max_packets);

    str("recording_directory", cfg.recording_directory);
    boolean("recording_enabled", cfg.recording_enabled);
    u64("recording_max_size", cfg.recording_max_size);
    u64("recording_queue_max_bytes", cfg.recording_queue_max_bytes);

    str("database_type", cfg.database_type);
    str("database_connection", cfg.database_connection);
    boolean("hls_high_scale_mode", cfg.hls_high_scale_mode);
    str("edge_viewer_stats_path", cfg.edge_viewer_stats_path);
    str("token_signing_secret", cfg.token_signing_secret);
    str("api_authentication_secret", cfg.api_authentication_secret);
    str("log_level", cfg.log_level);
    boolean("metrics_enabled", cfg.metrics_enabled);

    if (auto result = cfg.validate(); !result) {
        return result.error();
    }
    return cfg;
}

} // namespace rtmp_server::core
