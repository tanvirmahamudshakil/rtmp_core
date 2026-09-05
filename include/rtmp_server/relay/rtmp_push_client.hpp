#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/media_handoff_queue.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"
#include "rtmp_server/protocol/rtmp_url.hpp"

namespace rtmp_server::relay {

// Blocking, cancellation-aware RTMP **publishing** client: the mirror image of
// the source client, which plays. It owns the whole client side — DNS/TCP,
// simple handshake, connect / releaseStream / FCPublish / createStream /
// publish, chunk encoding, acknowledgement and ping response — and then
// forwards this server's own media out to another RTMP server.
//
// One class serves both jobs that need it:
//   * a **relay** (Wowza's "live stream repeater"): the target is another
//     StreamForge/RTMP origin, which re-fans the stream to its own viewers, so
//     ingest capacity is no longer bounded by one box;
//   * a **stream target**: the same push aimed at YouTube/Facebook/a CDN
//     ingest URL.
// The wire behaviour is identical; only the URL differs.
//
// Media is taken from a MediaHandoffQueue rather than pushed in, so the
// publisher's own thread never blocks on this connection and a slow or dead
// target costs the target's own quality, never the publish.
class RtmpPushClient {
public:
    using ContinuePredicate = std::function<bool()>;
    // Messages to send immediately after the target accepts the publish: the
    // stream's metadata and its current AVC/AAC sequence headers. Without them
    // a target that joined mid-stream cannot decode anything, and every
    // reconnect would produce a dead stream on the far side.
    using PrimingProvider = std::function<std::vector<media::HandoffMessage>()>;
    using PublishingHandler = std::function<void()>;

    struct Options {
        std::chrono::seconds connect_timeout{10};
        std::chrono::seconds command_timeout{15};
        // How long the target may go without acknowledging anything before the
        // connection is treated as dead. A publish is one-way, so this is the
        // only liveness signal short of a TCP error.
        std::chrono::seconds idle_timeout{30};
        std::uint32_t chunk_size = 4096;
        std::uint32_t max_message_size = 16u * 1024u * 1024u;
    };

    explicit RtmpPushClient(std::string target_url);
    RtmpPushClient(std::string target_url, Options options);
    ~RtmpPushClient();
    RtmpPushClient(const RtmpPushClient&) = delete;
    RtmpPushClient& operator=(const RtmpPushClient&) = delete;

    // Runs until `should_continue` returns false, the queue closes, or the
    // connection fails. Returns the failure so the caller can decide whether
    // to retry.
    [[nodiscard]] core::Result<void> run(const ContinuePredicate& should_continue,
                                         media::MediaHandoffQueue& queue,
                                         const PrimingProvider& priming,
                                         const PublishingHandler& on_publishing = {});

    // Bytes handed to the kernel for this target since the last run() started.
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return bytes_sent_; }

private:
    enum class State {
        Handshaking,
        Connecting,
        // connect() succeeded; the releaseStream/FCPublish/createStream
        // preamble is queued on the next socket turn.
        PreamblePending,
        CreatingStream,
        Publishing,
        Streaming,
        Failed
    };

    [[nodiscard]] core::Result<int> connect_socket(const protocol::RtmpUrl& parsed,
                                                   const ContinuePredicate& should_continue) const;
    void fail(core::ErrorCode code, core::ErrorCategory category, std::string message);
    void queue_bytes(std::span<const std::byte> bytes);
    void queue_message(const protocol::chunk::RtmpMessage& message);
    [[nodiscard]] bool drain_output();
    void send_connect(const protocol::RtmpUrl& parsed);
    void send_publish_preamble(const protocol::RtmpUrl& parsed);
    void send_publish(const protocol::RtmpUrl& parsed);
    void handle_message(protocol::chunk::RtmpMessage message);
    void handle_command(const protocol::chunk::RtmpMessage& message);
    void handle_user_control(const protocol::chunk::RtmpMessage& message);
    // Converts one queued media message into an RTMP message on the target's
    // stream, rebasing its timestamp so the target sees a stream that starts
    // near zero however long this server has been running.
    void send_media(const media::HandoffMessage& message);

    std::string target_url_;
    Options options_;
    int fd_ = -1;
    State state_ = State::Failed;
    protocol::chunk::ChunkEncoder encoder_;
    std::optional<protocol::chunk::ChunkDecoder> decoder_;
    std::vector<std::byte> handshake_input_;
    std::vector<std::byte> output_;
    std::size_t output_offset_ = 0;
    std::uint32_t message_stream_id_ = 0;
    double transaction_id_ = 1.0;
    std::optional<core::Error> error_;
    std::chrono::steady_clock::time_point state_deadline_{};
    std::chrono::steady_clock::time_point last_progress_{};
    media::TimestampUnwrapper clock_;
    std::optional<std::uint64_t> timestamp_base_;
    std::uint64_t bytes_sent_ = 0;
};

} // namespace rtmp_server::relay
