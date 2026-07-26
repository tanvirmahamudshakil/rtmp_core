#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/loadgen/media_source.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"

namespace rtmp_server::loadgen {

// A single REAL RTMP client over a real, non-blocking TCP socket
// (docs/v2_promot.md PHASE 7 "Real load generator": open real TCP
// connections, perform the RTMP handshake, send RTMP commands).
//
// This is the piece that makes the Phase 7 tool different in kind from the
// pre-Phase-7 apps/load_bench, which never opened a socket and instead
// called CommandSession::handle_message() in-process. Everything here goes
// over the wire: C0/C1/C2 handshake bytes, chunk-encoded AMF0 `connect` /
// `createStream` / `publish` / `play`, and chunk-encoded media messages.
//
// Design notes:
//
//  * Non-blocking + externally driven. The client owns no thread and never
//    blocks; a driver (ClientDriver) polls many of these over one poll(2)
//    set, so 1,000 viewers cost a handful of threads rather than 1,000.
//  * Partial writes are first-class (docs/v2_promot.md 3.4): pending output
//    lives in out_buffer_ with an explicit drain offset, and a short
//    ::send() is normal, not an error. EAGAIN/EINTR are retried; ECONNRESET/
//    EPIPE terminate the client cleanly.
//  * Bounded output. A client that cannot drain (the slow-viewer case) stops
//    generating rather than growing out_buffer_ without limit.
class RtmpClient {
public:
    enum class Role : std::uint8_t { Publisher, Viewer };

    enum class State : std::uint8_t {
        Connecting,        // TCP connect() in flight
        Handshaking,       // C0/C1 sent, waiting for S0/S1/S2
        Connecting_Rtmp,   // C2 + `connect` sent, waiting for _result
        CreatingStream,    // `createStream` sent, waiting for _result
        Starting,          // `publish`/`play` sent, waiting for onStatus
        Streaming,         // publishing media / receiving media
        Closed,
        Failed,
    };

    struct Config {
        std::string host = "127.0.0.1";
        std::uint16_t port = 1935;
        std::string application = "live";
        // For a publisher this is the publish secret / stream key; for a
        // viewer it is the playback name. NEVER logged in full — the tool
        // prints it via observability::redact().
        std::string stream_key;
        Role role = Role::Publisher;

        MediaProfile media;                    // publishers only
        std::uint32_t chunk_size = 4096;       // Set Chunk Size we advertise
        std::uint32_t max_message_size = 8u * 1024u * 1024u;

        // Bounded pending-output budget. Once out_buffer_ exceeds this a
        // publisher stops generating new frames for this tick (it does not
        // silently grow), which is exactly the backpressure a real encoder
        // experiences.
        std::size_t max_pending_out_bytes = 4u * 1024u * 1024u;

        // Slow-viewer simulation: read at most this many bytes per poll
        // cycle. 0 means "read everything available" (a healthy viewer). A
        // small value makes the client's receive window fill, the server's
        // socket buffer back up, and the server's per-viewer outbound queue
        // grow — which is how the slow-viewer policy is genuinely exercised
        // end-to-end rather than simulated in-process.
        std::size_t read_budget_per_tick = 0;
    };

    struct Stats {
        // Latencies, measured from the moment the operation was initiated.
        std::chrono::microseconds tcp_connect_latency{0};
        std::chrono::microseconds handshake_latency{0};
        std::chrono::microseconds connect_command_latency{0};
        std::chrono::microseconds publish_or_play_latency{0};
        // Viewer only: time from `play` acknowledgement to the first media
        // byte. This is the number that answers "how fast does a viewer
        // start?", which a synthetic in-process benchmark cannot measure.
        std::chrono::microseconds first_media_latency{0};

        std::uint64_t bytes_sent = 0;
        std::uint64_t bytes_received = 0;
        std::uint64_t media_messages_sent = 0;
        std::uint64_t media_messages_received = 0;
        std::uint64_t video_messages_received = 0;
        std::uint64_t audio_messages_received = 0;
        std::uint64_t keyframes_received = 0;

        // Corruption accounting: every received media payload is checked
        // against MediaSource::verify_pattern().
        std::uint64_t payloads_verified = 0;
        std::uint64_t payloads_corrupt = 0;

        // Transport-level observations.
        std::uint64_t partial_writes = 0;
        std::uint64_t reconnects = 0;
        bool reached_streaming = false;
        std::string failure_reason;
    };

    explicit RtmpClient(Config config);
    ~RtmpClient();

    RtmpClient(const RtmpClient&) = delete;
    RtmpClient& operator=(const RtmpClient&) = delete;
    RtmpClient(RtmpClient&&) = delete;
    RtmpClient& operator=(RtmpClient&&) = delete;

    // Opens a non-blocking socket and begins connect(2). Returns an error
    // only for immediate, local failures (socket/connect setup); an
    // in-progress connect is success.
    [[nodiscard]] core::Result<void> start();

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool finished() const noexcept { return state_ == State::Closed || state_ == State::Failed; }
    [[nodiscard]] bool wants_write() const noexcept;

    // Poll callbacks. Neither blocks.
    void on_readable();
    void on_writable();

    // Publisher only: generate and enqueue every media frame due at
    // `stream_time_ms`. No-op for viewers, before Streaming, or while the
    // pending-output budget is exhausted.
    void pump_media(std::uint32_t stream_time_ms);

    // Abrupt disconnect simulation: closes the socket immediately with no
    // RTMP teardown (no deleteStream/close), which is what a crashed encoder
    // or a yanked cable looks like to the server.
    void abort_connection();

    // Publisher reconnect simulation: abrupt close followed by a full fresh
    // connect/handshake/publish, restarting the media timeline.
    [[nodiscard]] core::Result<void> reconnect();

    // Graceful close: sends deleteStream, then closes.
    void close_gracefully();

private:
    void fail(std::string reason);
    void queue_out(std::span<const std::byte> bytes);
    void queue_message(const protocol::chunk::RtmpMessage& message);
    void drain_out();

    void finish_tcp_connect();
    void send_c0c1();
    void on_handshake_bytes(std::span<const std::byte> data);
    void send_c2_and_connect();
    void send_create_stream();
    void send_publish_or_play();
    void handle_message(const protocol::chunk::RtmpMessage& message);

    Config config_;
    State state_ = State::Closed;
    int fd_ = -1;

    Stats stats_;
    std::unique_ptr<MediaSource> media_;
    bool sent_sequence_headers_ = false;

    protocol::chunk::ChunkEncoder encoder_;
    std::unique_ptr<protocol::chunk::ChunkDecoder> decoder_;

    // Handshake scratch: S0/S1/S2 accumulate here until complete.
    std::vector<std::byte> handshake_in_;
    std::vector<std::byte> c1_sent_;

    std::vector<std::byte> out_buffer_;
    std::size_t out_offset_ = 0;

    std::uint32_t message_stream_id_ = 0;
    double transaction_id_ = 1.0;

    using Clock = std::chrono::steady_clock;
    Clock::time_point connect_started_{};
    Clock::time_point handshake_started_{};
    Clock::time_point connect_command_started_{};
    Clock::time_point start_command_started_{};
    Clock::time_point streaming_started_{};
};

} // namespace rtmp_server::loadgen
