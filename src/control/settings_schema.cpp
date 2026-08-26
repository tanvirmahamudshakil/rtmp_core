#include "rtmp_server/control/settings_schema.hpp"

namespace rtmp_server::control {

const std::vector<SettingField>& settings_schema() {
    using Type = SettingField::Type;
    static const std::vector<SettingField> kFields = {
        // --- Network ------------------------------------------------------
        {"rtmp_bind_address", "Network", "RTMP bind address",
         "Local address the RTMP listener binds to. 0.0.0.0 accepts publishers/viewers from any "
         "network interface; restrict it to a specific IP to only accept RTMP on one interface "
         "(e.g. a private VPN interface).",
         Type::String, false, true},
        {"rtmp_port", "Network", "RTMP port",
         "TCP port OBS/encoders publish to and RTMP players connect to. Changing this requires "
         "every encoder's ingest URL to be updated to match.",
         Type::U16, false, true},
        {"api_bind_address", "Network", "Admin API bind address",
         "Local address the management/admin HTTP API binds to. Keep this at 127.0.0.1 unless the "
         "admin API is meant to be reachable directly from outside this machine (normally a reverse "
         "proxy like Caddy handles that instead).",
         Type::String, false, true},
        {"api_port", "Network", "Admin API port",
         "TCP port the management/admin HTTP API listens on (what the admin panel and Caddy talk to).",
         Type::U16, false, true},
        {"public_rtmp_hostname", "Network", "Public RTMP hostname",
         "Hostname advertised to publishers in generated RTMP URLs (e.g. for the admin panel's "
         "\"copy stream URL\" button). Leave blank to let the server infer it from the request.",
         Type::String, false, true},

        // --- CPU & performance ---------------------------------------------
        {"worker_ring_count", "CPU & performance", "RTMP worker count",
         "Number of independent io_uring event-loop workers handling RTMP connections, each on its "
         "own thread. 0 = auto-detect from the machine's core count.",
         Type::U32, false, true},
        {"max_worker_ring_count", "CPU & performance", "Max RTMP worker count",
         "Upper bound the auto-detected/worker_ring_count value is clamped to, regardless of how "
         "many cores the box reports.",
         Type::U32, false, true},
        {"worker_cpu_pinning_enabled", "CPU & performance", "Pin RTMP workers to CPU cores",
         "Binds each RTMP worker thread to one specific core (round-robin) instead of letting the OS "
         "scheduler move it around freely. Can improve cache locality on a dedicated box; can hurt on "
         "a shared/cloud host with noisy-neighbour scheduling, so it defaults off.",
         Type::Bool, false, true},
        {"transcode_cpu_reservation_percent", "CPU & performance", "Transcoding CPU reservation (%)",
         "Percentage of the machine's cores reserved exclusively for source-transcode "
         "(scale/encode) work; RTMP ingest and the admin/HTTP API are confined to the remaining "
         "cores. 0 disables the split so everything shares every core, which is the historical "
         "default. Example: 60 on a 24-core box reserves ~14 cores for transcoding and leaves ~10 "
         "for everything else, so a busy control plane can never steal CPU from an in-flight encode.",
         Type::Percent, false, true},

        // --- io_uring tuning -------------------------------------------------
        {"ring_queue_depth", "io_uring tuning", "Ring queue depth",
         "Submission/completion queue depth for each io_uring instance. Higher values allow more "
         "in-flight I/O operations per worker at the cost of more kernel memory.",
         Type::U32, false, true},
        {"completion_batch_size", "io_uring tuning", "Completion batch size",
         "Maximum completions drained from the ring in one pass. Must not exceed ring_queue_depth.",
         Type::U32, false, true},
        {"submission_batch_size", "io_uring tuning", "Submission batch size",
         "Maximum submissions queued to the ring in one pass. Must not exceed ring_queue_depth.",
         Type::U32, false, true},
        {"enable_multishot_accept", "io_uring tuning", "Multishot accept",
         "Uses io_uring's multishot accept (one submission serves many incoming connections instead "
         "of resubmitting per-connection). Leave on unless debugging a kernel/io_uring issue.",
         Type::Bool, false, true},
        {"enable_multishot_recv", "io_uring tuning", "Multishot recv",
         "Uses io_uring's multishot recv for connection reads, cutting resubmission overhead per "
         "packet. Leave on unless debugging a kernel/io_uring issue.",
         Type::Bool, false, true},
        {"enable_registered_buffers", "io_uring tuning", "Registered buffers",
         "Pre-registers fixed I/O buffers with the kernel so io_uring skips per-call buffer pinning. "
         "Reduces per-packet CPU overhead under load.",
         Type::Bool, false, true},
        {"enable_provided_buffer_ring", "io_uring tuning", "Provided buffer ring",
         "Lets the kernel pick a buffer from a pre-supplied ring for each receive instead of the "
         "caller supplying one per call, reducing per-recv bookkeeping.",
         Type::Bool, false, true},
        {"enable_send_zero_copy", "io_uring tuning", "Zero-copy send",
         "Uses io_uring's SEND_ZEROCOPY for outgoing data (saves a copy of every byte pushed to "
         "viewers). Automatically falls back to a normal send if the running kernel/NIC rejects it.",
         Type::Bool, false, true},
        {"enable_sqpoll", "io_uring tuning", "Submission queue polling (SQPOLL)",
         "Runs a dedicated kernel thread that polls for new submissions instead of the app issuing a "
         "syscall per submission -- trades one CPU core (spinning while idle) for lower per-request "
         "latency. Only worth enabling if you have a core to spare.",
         Type::Bool, false, true},
        {"sqpoll_idle_ms", "io_uring tuning", "SQPOLL idle timeout (ms)",
         "How long the SQPOLL kernel thread keeps spinning with no work before it goes to sleep. "
         "Only relevant when enable_sqpoll is on.",
         Type::U32, false, true},
        {"registered_buffer_count", "io_uring tuning", "Registered buffer count",
         "Number of fixed buffers pre-registered with the kernel when enable_registered_buffers is on.",
         Type::U32, false, true},
        {"registered_buffer_size", "io_uring tuning", "Registered buffer size (bytes)",
         "Size of each pre-registered fixed buffer.",
         Type::U32, false, true},
        {"provided_buffer_count", "io_uring tuning", "Provided buffer count",
         "Number of buffers supplied to the kernel's provided-buffer ring when "
         "enable_provided_buffer_ring is on.",
         Type::U32, false, true},
        {"provided_buffer_size", "io_uring tuning", "Provided buffer size (bytes)",
         "Size of each buffer in the provided-buffer ring.",
         Type::U32, false, true},

        // --- Admission limits ------------------------------------------------
        {"maximum_connections", "Admission limits", "Max total connections",
         "Hard ceiling on simultaneous TCP connections across every RTMP worker. The default is "
         "unbounded (protection left entirely to the OS/hardware) -- set a real number to reject "
         "excess connections instead of letting the box degrade.",
         Type::U32, false, true},
        {"maximum_connections_per_ip", "Admission limits", "Max connections per IP",
         "Hard ceiling on simultaneous connections from a single source IP. Useful against a single "
         "misbehaving client or a small-scale flood.",
         Type::U32, false, true},
        {"maximum_publishers", "Admission limits", "Max concurrent publishers",
         "Hard ceiling on how many streams may be publishing (ingesting) at once.",
         Type::U32, false, true},
        {"maximum_viewers_per_stream", "Admission limits", "Max viewers per stream",
         "Hard ceiling on simultaneous RTMP viewers subscribed to one stream (HLS viewers behind "
         "Varnish are not counted here -- see hls_high_scale_mode).",
         Type::U32, false, true},

        // --- RTMP protocol -----------------------------------------------------
        {"input_chunk_size", "RTMP protocol", "Input chunk size",
         "RTMP chunk size the server advertises for data it receives from publishers.",
         Type::U32, false, true},
        {"output_chunk_size", "RTMP protocol", "Output chunk size",
         "RTMP chunk size the server advertises for data it sends to viewers.",
         Type::U32, false, true},
        {"maximum_rtmp_message_size", "RTMP protocol", "Max RTMP message size (bytes)",
         "Ceiling on one reassembled RTMP message. Protects the chunk-reassembly buffer from an "
         "unbounded allocation; real RTMP messages (even a keyframe) are normally well under 1 MiB.",
         Type::U32, false, true},
        {"handshake_timeout", "RTMP protocol", "Handshake timeout (ms)",
         "How long a connecting client has to complete the RTMP handshake before being dropped. "
         "Value is in milliseconds.",
         Type::DurationMs, false, true},
        {"authentication_timeout", "RTMP protocol", "Authentication timeout (ms)",
         "How long a connected client has to complete publish/play authentication before being "
         "dropped. Value is in milliseconds.",
         Type::DurationMs, false, true},
        {"idle_timeout", "RTMP protocol", "Idle timeout (ms)",
         "How long a connection may go without any traffic before being dropped as dead. Value is "
         "in milliseconds.",
         Type::DurationMs, false, true},
        {"write_timeout", "RTMP protocol", "Write timeout (ms)",
         "How long a write to a slow viewer may block before that viewer is dropped instead of "
         "stalling the connection indefinitely. Value is in milliseconds.",
         Type::DurationMs, false, true},
        {"publisher_inactivity_timeout", "RTMP protocol", "Publisher inactivity timeout (ms)",
         "How long a publisher may send no media before being treated as having stopped streaming. "
         "Value is in milliseconds.",
         Type::DurationMs, false, true},

        // --- Live buffering --------------------------------------------------
        {"gop_cache_max_duration", "Live buffering", "GOP cache max duration (ms)",
         "How much recent video a stream keeps buffered so a newly joining RTMP viewer can start "
         "from the last keyframe instead of waiting for the next one. Value is in milliseconds.",
         Type::DurationMs, false, true},
        {"gop_cache_max_bytes", "Live buffering", "GOP cache max size (bytes)",
         "Byte ceiling on the same keyframe-catchup buffer, independent of its duration ceiling.",
         Type::U64, false, true},
        {"gop_cache_max_packets", "Live buffering", "GOP cache max packets",
         "Packet-count ceiling on the same keyframe-catchup buffer.",
         Type::U32, false, true},
        {"subscriber_queue_max_bytes", "Live buffering", "Viewer queue max size (bytes)",
         "Per-viewer outbound queue byte ceiling. A viewer whose connection can't keep up is dropped "
         "once its queue exceeds this instead of the queue growing without bound.",
         Type::U64, false, true},
        {"subscriber_queue_max_packets", "Live buffering", "Viewer queue max packets",
         "Per-viewer outbound queue packet-count ceiling, alongside the byte ceiling above.",
         Type::U32, false, true},

        // --- Recording -----------------------------------------------------
        {"recording_enabled", "Recording", "Enable recording",
         "Turns on recording published streams to disk (per-stream recording can still be toggled "
         "individually; this is the global switch).",
         Type::Bool, false, true},
        {"recording_directory", "Recording", "Recording directory",
         "Filesystem path recordings are written under.",
         Type::String, false, true},
        {"recording_max_size", "Recording", "Recording max file size (bytes)",
         "A recording still in progress is rotated to a new file once it reaches this size.",
         Type::U64, false, true},
        {"recording_queue_max_bytes", "Recording", "Recording write queue max size (bytes)",
         "Buffer ceiling for data waiting to be flushed to disk. Protects memory if the disk falls "
         "behind the incoming stream.",
         Type::U64, false, true},

        // --- HLS delivery ----------------------------------------------------
        {"enable_hls_fast_join", "HLS delivery", "HLS fast join",
         "Redirects a fresh master.m3u8 open straight to the lowest-bitrate rendition instead of "
         "serving the master playlist first, skipping one round trip before playback starts.",
         Type::Bool, false, true},
        {"hls_high_scale_mode", "HLS delivery", "HLS high-scale (shared cache) mode",
         "Serves HLS through a shared-cache-friendly path with no per-viewer redirect/query state, "
         "so a reverse cache (e.g. Varnish) can collapse many viewers' playlist polls into one origin "
         "request. Turn off only for a small deployment where per-viewer session tracking without a "
         "shared cache in front is preferred.",
         Type::Bool, false, true},
        {"edge_viewer_stats_path", "HLS delivery", "Edge viewer stats file path",
         "Where the cache-edge viewer/bandwidth accounting file (written by "
         "deploy/viewer-estimator) is read from. Empty disables reading it, and every stream then "
         "reports only what the origin itself can see -- which undercounts viewers once a shared "
         "cache is in front, since it never sees most viewer requests.",
         Type::String, false, true},

        // --- Storage ---------------------------------------------------------
        {"database_type", "Storage", "Database type",
         "Persistence backend for stream/application/source-job configuration. Currently only "
         "\"sqlite\" is supported.",
         Type::String, false, true},
        {"database_connection", "Storage", "Database path",
         "Filesystem path to the SQLite database file.",
         Type::String, false, true},

        // --- Security ---------------------------------------------------------
        {"token_signing_secret", "Security", "Token signing secret",
         "HMAC key used to sign/verify playback and publish tokens. Must be at least 32 random "
         "characters and different from the admin API secret below. Changing this immediately "
         "invalidates every token issued under the old value.",
         Type::String, true, true},
        {"api_authentication_secret", "Security", "Admin API bearer token",
         "Bearer token required on management/admin API requests. Must be at least 32 random "
         "characters and different from the token signing secret above. Changing this immediately "
         "logs out every client using the old token, including this admin panel.",
         Type::String, true, true},

        // --- Observability ---------------------------------------------------
        {"log_level", "Observability", "Log level",
         "Minimum severity written to the server log (e.g. debug, info, warn, error).",
         Type::String, false, true},
        {"metrics_enabled", "Observability", "Enable Prometheus metrics",
         "Turns the /metrics endpoint on or off.",
         Type::Bool, false, true},
    };
    return kFields;
}

} // namespace rtmp_server::control
