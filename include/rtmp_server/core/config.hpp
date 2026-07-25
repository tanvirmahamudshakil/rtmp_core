#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/core/result.hpp"

namespace rtmp_server::core {

// Mirrors the required keys documented in docs/rtmp_promot.md "Configuration"
// and config/server.example.yaml. Loaded from YAML with env-var and CLI
// overrides layered on top (highest precedence: CLI > env > file > default).
struct ServerConfig {
    std::string rtmp_bind_address = "0.0.0.0";
    std::uint16_t rtmp_port = 1935;
    std::string api_bind_address = "127.0.0.1";
    std::uint16_t api_port = 8080;
    std::string public_rtmp_hostname;

    std::uint32_t ring_queue_depth = 1024;
    std::uint32_t completion_batch_size = 64;
    std::uint32_t submission_batch_size = 64;
    std::uint32_t worker_ring_count = 1;

    bool enable_multishot_accept = true;
    bool enable_multishot_recv = true;
    bool enable_registered_buffers = true;
    bool enable_provided_buffer_ring = true;
    bool enable_send_zero_copy = false;
    bool enable_sqpoll = false;
    std::uint32_t sqpoll_idle_ms = 1000;

    std::uint32_t registered_buffer_count = 1024;
    std::uint32_t registered_buffer_size = 65536;
    std::uint32_t provided_buffer_count = 4096;
    std::uint32_t provided_buffer_size = 65536;

    std::uint32_t maximum_connections = 10000;
    std::uint32_t maximum_connections_per_ip = 50;
    std::uint32_t maximum_publishers = 1000;
    std::uint32_t maximum_viewers_per_stream = 5000;

    std::uint32_t input_chunk_size = 128;
    std::uint32_t output_chunk_size = 4096;
    std::uint32_t maximum_rtmp_message_size = 10 * 1024 * 1024;

    std::chrono::milliseconds handshake_timeout{5000};
    std::chrono::milliseconds authentication_timeout{5000};
    std::chrono::milliseconds idle_timeout{60000};
    std::chrono::milliseconds write_timeout{10000};
    std::chrono::milliseconds publisher_inactivity_timeout{30000};

    std::chrono::milliseconds gop_cache_max_duration{10000};
    std::uint64_t gop_cache_max_bytes = 16 * 1024 * 1024;
    std::uint32_t gop_cache_max_packets = 2000;

    std::uint64_t subscriber_queue_max_bytes = 8 * 1024 * 1024;
    std::uint32_t subscriber_queue_max_packets = 1000;

    std::string recording_directory = "./recordings";
    bool recording_enabled = false;
    std::uint64_t recording_max_size = 4ULL * 1024 * 1024 * 1024;
    std::uint64_t recording_queue_max_bytes = 16 * 1024 * 1024;

    std::string database_type = "sqlite";
    std::string database_connection = "./data/rtmp.db";

    std::string token_signing_secret;
    std::string api_authentication_secret;

    std::string log_level = "info";
    bool metrics_enabled = true;

    // Validates invariants that cannot be expressed in the type system
    // (non-empty secrets, sane port ranges, positive limits). Called once at
    // startup; the server must fail fast on invalid configuration rather
    // than run with dangerous defaults (see docs/rtmp_promot.md
    // "Never silently accept dangerous default secrets").
    [[nodiscard]] Result<void> validate() const;
};

// Loads a ServerConfig from a YAML file at `path`, then applies RTMP_SERVER_*
// environment variable overrides. CLI argument parsing happens in the
// executable's main() and is layered on top of the returned config.
[[nodiscard]] Result<ServerConfig> load_config(const std::string& path);

} // namespace rtmp_server::core
