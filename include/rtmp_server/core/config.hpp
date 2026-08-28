#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/core/result.hpp"

namespace rtmp_server::core {

// Phase 8 release-gate constants (see ServerConfig::validate).

// Minimum length for token_signing_secret / api_authentication_secret. 32
// characters matches the HMAC-SHA256 key size the token scheme uses: shorter
// keys are brute-forceable offline from a single observed token, longer ones
// buy nothing. Generate with `openssl rand -hex 32`.
inline constexpr std::size_t kMinSecretLength = 32;

// Ceiling on maximum_rtmp_message_size. The per-connection chunk reassembly
// budget is derived from this value, so an unbounded setting would defeat the
// bounds in protocol/chunk/chunk_decoder.hpp. 64 MiB is far above any real
// RTMP message (the largest thing a publisher sends is a keyframe, typically
// well under 1 MiB even at high bitrates).
inline constexpr std::uint32_t kMaxSupportedRtmpMessageSize = 64u * 1024u * 1024u;

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

    // Phase 4 (docs/v2_promot.md "Multi-core io_uring worker architecture"):
    // number of independent IoUringEventLoop workers to run, each with its
    // own ring/connections/buffers, bound to the same port via
    // SO_REUSEPORT. 0 means "auto": use std::thread::hardware_concurrency(),
    // clamped to [1, max_worker_ring_count]. A positive value is used as-is,
    // also clamped to max_worker_ring_count (task "worker_ring_count
    // actually control worker creation" + "default based on hardware
    // concurrency, with a configurable upper bound").
    std::uint32_t worker_ring_count = 0;
    // Raised from the original 64 so a high-core-count box isn't artificially
    // capped below its real core count; SO_REUSEPORT/io_uring ring setup is
    // the real ceiling per worker (fd/memory), not this number.
    std::uint32_t max_worker_ring_count = 4096;

    // Pins worker i to CPU (i % hardware_concurrency) via
    // pthread_setaffinity_np when true. Optional per task 12 ("must not be
    // mandatory") — default off since pinning can hurt on shared/cloud hosts
    // with noisy-neighbour scheduling.
    bool worker_cpu_pinning_enabled = false;

    // Percentage (0-100) of the machine's cores reserved exclusively for
    // source-transcode work; the remainder is what RTMP ingest workers pin
    // to. 0 (the default) disables the split -- ingest workers keep pinning
    // (if worker_cpu_pinning_enabled) across every core, unchanged from
    // before this option existed. Mirrors
    // transcoding::native::SourceJobOptions::transcode_cpu_reservation_percent;
    // main() reads one operator-facing value and threads it to both.
    std::uint32_t transcode_cpu_reservation_percent = 0;

    bool enable_multishot_accept = true;
    bool enable_multishot_recv = true;
    bool enable_registered_buffers = true;
    bool enable_provided_buffer_ring = true;
    // Capability-gated and automatically falls back to ordinary io_uring
    // send if the running kernel/NIC rejects SEND_ZC.
    bool enable_send_zero_copy = true;
    bool enable_sqpoll = false;
    std::uint32_t sqpoll_idle_ms = 1000;

    std::uint32_t registered_buffer_count = 1024;
    std::uint32_t registered_buffer_size = 65536;
    std::uint32_t provided_buffer_count = 4096;
    std::uint32_t provided_buffer_size = 65536;

    // Unbounded by request. These were the operator-facing admission caps
    // (docs/v2_promot.md section 3.5's "remote-controlled resource must have
    // a real bound") -- setting them to the type maximum removes that
    // protection: a connection/publish/viewer flood is no longer rejected at
    // any count, only by whatever the OS/hardware eventually can't sustain.
    // validate() still requires these to be non-zero, which UINT32_MAX
    // satisfies.
    std::uint32_t maximum_connections = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_connections_per_ip = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_publishers = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_viewers_per_stream = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t input_chunk_size = 128;
    std::uint32_t output_chunk_size = 4096;
    std::uint32_t maximum_rtmp_message_size = 10 * 1024 * 1024;

    // "Fast join": a fresh open of any stream's master.m3u8 is redirected
    // straight to its lowest-bitrate rendition instead of serving the master
    // playlist, skipping the variant-negotiation round trip. Applies to
    // every stream generically -- see HlsHttpOptions::enable_fast_join.
    bool enable_hls_fast_join = false;

    std::chrono::milliseconds handshake_timeout{5000};
    std::chrono::milliseconds authentication_timeout{5000};
    std::chrono::milliseconds idle_timeout{60000};
    std::chrono::milliseconds write_timeout{10000};
    std::chrono::milliseconds publisher_inactivity_timeout{30000};

    std::chrono::milliseconds gop_cache_max_duration{10000};
    std::uint64_t gop_cache_max_bytes = 16 * 1024 * 1024;
    std::uint32_t gop_cache_max_packets = 2000;

    // A few seconds of common live-video bitrate: enough for transient
    // jitter, but small enough that thousands of slow viewers cannot retain
    // many GiB before keyframe recovery/eviction activates.
    std::uint64_t subscriber_queue_max_bytes = 4 * 1024 * 1024;
    std::uint32_t subscriber_queue_max_packets = 512;

    std::string recording_directory = "./recordings";
    bool recording_enabled = false;
    std::uint64_t recording_max_size = 4ULL * 1024 * 1024 * 1024;
    std::uint64_t recording_queue_max_bytes = 16 * 1024 * 1024;

    std::string database_type = "sqlite";
    std::string database_connection = "./data/rtmp.db";

    // Shared-cache HLS delivery: no per-viewer redirect/query state and no
    // per-delivery origin accounting. This is the production default for a
    // single VPS serving a very large public audience through local Varnish.
    bool hls_high_scale_mode = true;

    // Where the cache edge publishes its per-link viewer/delivery accounting
    // (deploy/viewer-estimator writes this every 2s from varnishncsa). The
    // origin cannot count HLS viewers itself -- Varnish collapses a thousand
    // players' polls into roughly one origin request per second -- so this
    // file is the only source of a real per-link viewer count. Empty turns
    // the reader off, and every link then reports only what the origin can
    // see for itself. See control::EdgeViewerStats.
    std::string edge_viewer_stats_path = "/var/www/streamforge/internal/viewer_estimate.json";

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
