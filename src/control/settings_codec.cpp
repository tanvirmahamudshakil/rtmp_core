#include "rtmp_server/control/settings_codec.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "rtmp_server/control/settings_schema.hpp"

namespace rtmp_server::control {

namespace {

using core::Error;
using core::ErrorCategory;
using core::ErrorCode;
using core::Result;
using core::ServerConfig;

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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Same flat "key: value" shape as core::config.cpp's own loader (comments
// start with '#', values may be quoted). Duplicated rather than shared
// because that parser is file-local to config.cpp; this file only needs it
// to preserve unrecognised lines across a rewrite, not to build a
// ServerConfig itself (core::load_config still does that, for validation).
std::unordered_map<std::string, std::string> parse_flat_mapping(const std::string& text) {
    std::unordered_map<std::string, std::string> out;
    std::istringstream in(text);
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
        out[std::move(key)] = std::move(value);
    }
    return out;
}

std::string serialize_flat_mapping(const std::unordered_map<std::string, std::string>& values) {
    std::ostringstream out;
    out << "# Managed by the StreamForge admin Settings page.\n";
    out << "# Comments and key ordering are not preserved across a save from that page.\n";
    for (const auto& field : settings_schema()) {
        auto it = values.find(field.key);
        if (it == values.end()) continue;
        out << field.key << ": \"" << it->second << "\"\n";
    }
    // Any key the schema doesn't know about (forward/backward compat with a
    // config file edited by hand) is preserved verbatim instead of dropped.
    for (const auto& [key, value] : values) {
        bool known = false;
        for (const auto& field : settings_schema()) {
            if (field.key == key) {
                known = true;
                break;
            }
        }
        if (!known) out << key << ": \"" << value << "\"\n";
    }
    return out.str();
}

std::string field_value_string(const ServerConfig& cfg, const SettingField& field) {
    // One key per ServerConfig field this schema entry describes; mirrors
    // config.cpp's load-side str/u32/u64/boolean/duration calls in reverse.
    // clang-format off
    if (field.key == "rtmp_bind_address") return cfg.rtmp_bind_address;
    if (field.key == "rtmp_port") return std::to_string(cfg.rtmp_port);
    if (field.key == "api_bind_address") return cfg.api_bind_address;
    if (field.key == "api_port") return std::to_string(cfg.api_port);
    if (field.key == "public_rtmp_hostname") return cfg.public_rtmp_hostname;
    if (field.key == "ring_queue_depth") return std::to_string(cfg.ring_queue_depth);
    if (field.key == "completion_batch_size") return std::to_string(cfg.completion_batch_size);
    if (field.key == "submission_batch_size") return std::to_string(cfg.submission_batch_size);
    if (field.key == "worker_ring_count") return std::to_string(cfg.worker_ring_count);
    if (field.key == "max_worker_ring_count") return std::to_string(cfg.max_worker_ring_count);
    if (field.key == "worker_cpu_pinning_enabled") return cfg.worker_cpu_pinning_enabled ? "true" : "false";
    if (field.key == "transcode_cpu_reservation_percent") return std::to_string(cfg.transcode_cpu_reservation_percent);
    if (field.key == "enable_multishot_accept") return cfg.enable_multishot_accept ? "true" : "false";
    if (field.key == "enable_multishot_recv") return cfg.enable_multishot_recv ? "true" : "false";
    if (field.key == "enable_registered_buffers") return cfg.enable_registered_buffers ? "true" : "false";
    if (field.key == "enable_provided_buffer_ring") return cfg.enable_provided_buffer_ring ? "true" : "false";
    if (field.key == "enable_send_zero_copy") return cfg.enable_send_zero_copy ? "true" : "false";
    if (field.key == "enable_sqpoll") return cfg.enable_sqpoll ? "true" : "false";
    if (field.key == "sqpoll_idle_ms") return std::to_string(cfg.sqpoll_idle_ms);
    if (field.key == "registered_buffer_count") return std::to_string(cfg.registered_buffer_count);
    if (field.key == "registered_buffer_size") return std::to_string(cfg.registered_buffer_size);
    if (field.key == "provided_buffer_count") return std::to_string(cfg.provided_buffer_count);
    if (field.key == "provided_buffer_size") return std::to_string(cfg.provided_buffer_size);
    if (field.key == "maximum_connections") return std::to_string(cfg.maximum_connections);
    if (field.key == "maximum_connections_per_ip") return std::to_string(cfg.maximum_connections_per_ip);
    if (field.key == "maximum_publishers") return std::to_string(cfg.maximum_publishers);
    if (field.key == "maximum_viewers_per_stream") return std::to_string(cfg.maximum_viewers_per_stream);
    if (field.key == "input_chunk_size") return std::to_string(cfg.input_chunk_size);
    if (field.key == "output_chunk_size") return std::to_string(cfg.output_chunk_size);
    if (field.key == "maximum_rtmp_message_size") return std::to_string(cfg.maximum_rtmp_message_size);
    if (field.key == "enable_hls_fast_join") return cfg.enable_hls_fast_join ? "true" : "false";
    if (field.key == "handshake_timeout") return std::to_string(cfg.handshake_timeout.count());
    if (field.key == "authentication_timeout") return std::to_string(cfg.authentication_timeout.count());
    if (field.key == "idle_timeout") return std::to_string(cfg.idle_timeout.count());
    if (field.key == "write_timeout") return std::to_string(cfg.write_timeout.count());
    if (field.key == "publisher_inactivity_timeout") return std::to_string(cfg.publisher_inactivity_timeout.count());
    if (field.key == "gop_cache_max_duration") return std::to_string(cfg.gop_cache_max_duration.count());
    if (field.key == "gop_cache_max_bytes") return std::to_string(cfg.gop_cache_max_bytes);
    if (field.key == "gop_cache_max_packets") return std::to_string(cfg.gop_cache_max_packets);
    if (field.key == "subscriber_queue_max_bytes") return std::to_string(cfg.subscriber_queue_max_bytes);
    if (field.key == "subscriber_queue_max_packets") return std::to_string(cfg.subscriber_queue_max_packets);
    if (field.key == "recording_directory") return cfg.recording_directory;
    if (field.key == "recording_enabled") return cfg.recording_enabled ? "true" : "false";
    if (field.key == "recording_max_size") return std::to_string(cfg.recording_max_size);
    if (field.key == "recording_queue_max_bytes") return std::to_string(cfg.recording_queue_max_bytes);
    if (field.key == "database_type") return cfg.database_type;
    if (field.key == "database_connection") return cfg.database_connection;
    if (field.key == "hls_high_scale_mode") return cfg.hls_high_scale_mode ? "true" : "false";
    if (field.key == "edge_viewer_stats_path") return cfg.edge_viewer_stats_path;
    if (field.key == "token_signing_secret") return cfg.token_signing_secret;
    if (field.key == "api_authentication_secret") return cfg.api_authentication_secret;
    if (field.key == "log_level") return cfg.log_level;
    if (field.key == "metrics_enabled") return cfg.metrics_enabled ? "true" : "false";
    // clang-format on
    return "";
}

std::string type_name(SettingField::Type type) {
    switch (type) {
        case SettingField::Type::String: return "string";
        case SettingField::Type::Bool: return "bool";
        case SettingField::Type::U16: return "u16";
        case SettingField::Type::U32: return "u32";
        case SettingField::Type::U64: return "u64";
        case SettingField::Type::DurationMs: return "duration_ms";
        case SettingField::Type::Percent: return "percent";
    }
    return "string";
}

std::string build_settings_json(const ServerConfig& cfg) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& field : settings_schema()) {
        if (!first) out << ",";
        first = false;
        out << R"({"key":")" << json_escape(field.key) << R"(","section":")" << json_escape(field.section)
            << R"(","label":")" << json_escape(field.label) << R"(","description":")"
            << json_escape(field.description) << R"(","type":")" << type_name(field.type)
            << R"(","restart_required":)" << (field.restart_required ? "true" : "false")
            << R"(,"sensitive":)" << (field.sensitive ? "true" : "false");
        if (field.sensitive) {
            const bool has_value = !field_value_string(cfg, field).empty();
            out << R"(,"has_value":)" << (has_value ? "true" : "false");
        } else {
            out << R"(,"value":")" << json_escape(field_value_string(cfg, field)) << "\"";
        }
        out << "}";
    }
    out << "]";
    return out.str();
}

Result<std::string> read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Error(ErrorCode::MissingConfiguration, ErrorCategory::Configuration,
                     "config file not found: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace

Result<std::string> settings_to_json(const std::string& config_path) {
    auto cfg = core::load_config(config_path);
    if (!cfg) return cfg.error();
    return build_settings_json(cfg.value());
}

Result<std::string> apply_settings_updates(const std::string& config_path,
                                           const std::unordered_map<std::string, std::string>& updates) {
    auto existing = read_file(config_path);
    if (!existing) return existing.error();

    auto merged = parse_flat_mapping(existing.value());

    static const std::unordered_map<std::string, SettingField::Type> kKnownKeys = [] {
        std::unordered_map<std::string, SettingField::Type> keys;
        for (const auto& field : settings_schema()) keys.emplace(field.key, field.type);
        return keys;
    }();

    for (const auto& [key, value] : updates) {
        const auto known = kKnownKeys.find(key);
        if (known == kKnownKeys.end()) {
            return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                         "unknown setting: " + key);
        }
        // core::load_config's duration parser requires a unit suffix
        // (ms/s/m/h) — the admin UI's plain number input sends a bare
        // millisecond count, so add the suffix here rather than making every
        // caller of this function (and every hand-edit of the config file
        // through this endpoint) know about that parser's syntax.
        if (known->second == SettingField::Type::DurationMs && !value.empty() &&
            value.find_first_not_of("0123456789") == std::string::npos) {
            merged[key] = value + "ms";
        } else {
            merged[key] = value;
        }
    }

    const std::string temp_path = config_path + ".tmp";
    {
        std::ofstream temp(temp_path, std::ios::trunc);
        if (!temp.is_open()) {
            return Error(ErrorCode::Unknown, ErrorCategory::Internal,
                         "could not open " + temp_path + " for writing");
        }
        temp << serialize_flat_mapping(merged);
    }

    auto validated = core::load_config(temp_path);
    if (!validated) {
        std::remove(temp_path.c_str());
        return validated.error();
    }

    if (std::rename(temp_path.c_str(), config_path.c_str()) != 0) {
        std::remove(temp_path.c_str());
        return Error(ErrorCode::Unknown, ErrorCategory::Internal,
                     "could not replace " + config_path + " with validated settings");
    }

    return build_settings_json(validated.value());
}

} // namespace rtmp_server::control
