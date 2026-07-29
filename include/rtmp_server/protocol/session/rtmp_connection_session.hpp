#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/core/buffer.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"
#include "rtmp_server/protocol/commands/command_session.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/recorder_sink.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"
#include "rtmp_server/protocol/media/media_ingest.hpp"

namespace rtmp_server::protocol::session {

// Post-handshake RTMP pipeline for exactly one connection (Phase 1,
// docs/v2_promot.md "PHASE 1 — Complete RTMP connection and session
// pipeline"). Owns and wires together:
//
//   ChunkDecoder -> CommandSession (AMF connect/createStream/publish/play/
//                   deleteStream, media routing) -> ChunkEncoder
//
// plus the RTMP protocol-control messages that live above the chunk layer
// (Window Acknowledgement Size / Set Peer Bandwidth handshake-completion
// messages, Acknowledgement replies, User Control "Stream Begin", and
// Ping Request/Response) which chunk::ChunkDecoder deliberately does not
// own (see chunk_decoder.hpp: "Acknowledgement and Set Peer Bandwidth ...
// simply delivered to the message handler ... session-level concern").
//
// Deliberately transport-agnostic, exactly like HandshakeSession/
// ChunkDecoder/CommandSession: no socket, no io_uring. Driven by
// on_bytes_received() (fed from TcpConnection::receive_handler_ once the
// handshake completes) and produces already chunk-encoded bytes via the
// outgoing byte handler, which the transport layer writes to the socket.
// This split is what makes the "Required tests" (docs/v2_promot.md Phase 1)
// runnable on any platform, including macOS where the io_uring transport
// itself cannot build/run (CMAKE_SYSTEM_NAME STREQUAL "Linux" guard).
class RtmpConnectionSession {
public:
    using OutgoingBytesHandler = std::function<void(std::vector<std::byte>)>;
    using SharedOutgoingBytesHandler = std::function<void(core::SharedBuffer)>;
    using CloseHandler = std::function<void()>; // fatal protocol error -> caller should close the socket

    struct Dependencies {
        commands::StreamRegistry* registry = nullptr;         // required, not owned
        commands::StreamIdRegistry* stream_id_registry = nullptr; // optional, not owned; falls back to a private one
        commands::LiveFanout* live_fanout = nullptr;          // optional, not owned
        media::MediaIngest* media_ingest = nullptr;           // optional, not owned
        commands::RecorderSink* recorder = nullptr;           // optional, not owned
        commands::RecorderFactory recorder_factory;           // optional; per-publish owned sink
        commands::StreamKeyValidator key_validator;           // optional; defaults to "always allow"
        commands::StreamIdResolver stream_id_resolver;         // optional; raw publish key -> public stream name
        commands::PlaybackAuthorizer playback_authorizer;      // optional; stream/token/IP playback gate
        commands::ViewerLifecycleHandler viewer_attached_handler;
        commands::ViewerLifecycleHandler viewer_detached_handler;
        commands::CommandSession::PublishStartHandler publish_start_handler;
        commands::CommandSession::PublishStopHandler publish_stop_handler;
        std::string client_ip;                                 // optional; forwarded to playback authorization
        commands::QueueLimits playback_queue_limits;           // optional; defaults per viewer_queue.hpp
    };

    RtmpConnectionSession(std::uint64_t connection_id, Dependencies deps, std::uint32_t max_message_size,
                           std::uint32_t output_chunk_size);

    RtmpConnectionSession(const RtmpConnectionSession&) = delete;
    RtmpConnectionSession& operator=(const RtmpConnectionSession&) = delete;

    void set_outgoing_handler(OutgoingBytesHandler handler) { outgoing_handler_ = std::move(handler); }
    // Preferred transport path: immutable encoded buffers can be referenced
    // by thousands of TcpConnection queues without duplicating their bytes.
    // The vector handler remains for portable embedders/tests.
    void set_shared_outgoing_handler(SharedOutgoingBytesHandler handler) {
        shared_outgoing_handler_ = std::move(handler);
    }
    void set_close_handler(CloseHandler handler) { close_handler_ = std::move(handler); }

    // Must be called once, after set_outgoing_handler(), before any bytes
    // are fed via on_bytes_received(). Sends the peer a Set Chunk Size
    // control message when this session's ChunkEncoder was constructed with
    // a non-default chunk size (Phase 1 task 9). Without this, the peer's
    // decoder keeps assuming chunk::kDefaultChunkSize (128) and cannot
    // correctly reassemble any message this session encodes with fewer,
    // larger chunks — a real interoperability bug this test suite caught:
    // see RtmpFullSessionSocketIntegration.HandshakePlusConnectInSameWrite-
    // ReachesTheSession, which stalls forever without this call.
    void start();

    // Reports this connection's current outbound socket backlog in bytes,
    // used by CommandSession for playback backpressure (see
    // command_session.hpp set_pending_bytes_provider). Optional.
    void set_pending_bytes_provider(std::function<std::size_t()> provider);
    void set_pending_queue_provider(std::function<commands::QueueBacklog()> provider);
    void set_max_queued_playback_bytes(std::size_t bytes);

    // Feeds a fragment of bytes read off the wire after the RTMP handshake
    // completed, in order — including any bytes the handshake layer had
    // already buffered alongside C2 (see HandshakeSession::take_trailing_
    // bytes()). May be called any number of times with arbitrary
    // fragmentation. No-op once failed().
    void on_bytes_received(std::span<const std::byte> data);

    // Phase 7 observability. Non-owning, optional, must outlive the session.
    // Feeds ingress_bytes_total from the single point every inbound RTMP byte
    // passes through.
    void set_metrics(observability::Metrics* metrics) noexcept { metrics_ = metrics; }

    // Must be called exactly once by the connection owner on socket
    // disconnect (regardless of state — mid-handshake reuse is not this
    // class's concern) so any publisher/viewer registration this session
    // holds is torn down deterministically (Phase 1 task 12).
    void on_connection_closed();

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] commands::CommandSession& command_session() noexcept { return command_session_; }

private:
    void handle_decoded_message(chunk::RtmpMessage message);
    void handle_decode_error(core::Error error);
    void send_encoded(const chunk::RtmpMessage& message);
    void send_encoded_media(const commands::SharedMediaFrame& frame,
                            std::uint32_t chunk_stream_id,
                            std::uint32_t message_stream_id);
    void emit_encoded(std::vector<std::byte> bytes);
    void flush_pending_acknowledgement();
    void handle_user_control(const chunk::RtmpMessage& message);
    void fail(std::string_view reason);

    [[maybe_unused]] std::uint64_t connection_id_; // retained for future diagnostics/logging use
    chunk::ChunkDecoder decoder_;
    chunk::ChunkEncoder encoder_;
    std::uint32_t output_chunk_size_;
    commands::CommandSession command_session_;
    OutgoingBytesHandler outgoing_handler_;
    SharedOutgoingBytesHandler shared_outgoing_handler_;
    CloseHandler close_handler_;
    std::vector<std::byte> out_scratch_;
    bool failed_ = false;
    observability::Metrics* metrics_ = nullptr; // not owned, may be null
    bool closed_ = false;
    bool started_ = false;
};

} // namespace rtmp_server::protocol::session
