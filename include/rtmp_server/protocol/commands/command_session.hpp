#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include <memory>

#include "rtmp_server/protocol/amf0/amf0_value.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/recorder_sink.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"
#include "rtmp_server/protocol/media/media_ingest.hpp"

namespace rtmp_server::protocol::commands {

// Lifecycle state of one RTMP message stream (createStream-allocated
// stream ID) within a connection. A connection may have several of these
// concurrently (RTMP allows multiple createStream calls per NetConnection),
// though in practice OBS/typical publishers use exactly one.
enum class NetStreamState : std::uint8_t {
    Idle,      // created, not yet publish()'d or play()'d
    Publishing,
    Playing,
};

// Injectable stream-key authorization: given the app name from `connect`
// and the stream key from `publish`, returns whether this publish attempt
// is allowed. Kept as a std::function (rather than a hardcoded check) so
// the actual policy (signed-URL validation, database lookup, static
// allow-list, always-true for tests, ...) is a deployment/test concern, not
// something CommandSession bakes in — mirrors how HandshakeSession and
// ChunkDecoder take handlers instead of owning transport or timer logic.
using StreamKeyValidator = std::function<bool(std::string_view app, std::string_view stream_key)>;

// Parsed view of one decoded AMF0 command message (type 20). Exposed
// mainly for tests; CommandSession::handle_message() does this parsing
// internally for real traffic.
struct Amf0Command {
    std::string name;
    double transaction_id = 0.0;
    amf0::Amf0Value command_object = amf0::Amf0Value::null();
    std::vector<amf0::Amf0Value> arguments;
};

// Per-connection RTMP command dispatcher, built on top of
// chunk::ChunkDecoder/ChunkEncoder exactly like protocol::handshake::
// HandshakeSession is built on top of raw handshake bytes: pure protocol
// logic, no sockets, no io_uring, driven by already-reassembled
// chunk::RtmpMessage values and producing chunk::RtmpMessage values in
// return via an outgoing-message handler (docs/architecture.md
// "Architectural Separation", docs/rtmp-commands.md has the full write-up).
//
// Handles: connect, releaseStream, FCPublish, createStream, publish, play,
// deleteStream. Responses use the standard AMF0 command-message shapes
// RTMP clients (OBS, ffmpeg, VLC, ...) expect: `_result`/`_error` for
// NetConnection-level and stream-creation commands, `onStatus` for
// NetStream-level status changes (NetConnection.Connect.Success,
// NetStream.Publish.Start, NetStream.Publish.BadName,
// NetStream.Play.Start, ...).
class CommandSession {
public:
    using OutgoingHandler = std::function<void(chunk::RtmpMessage)>;

    // Invoked once a `publish` for a previously-unpublished, validator-approved
    // stream key succeeds and the stream has been added to the registry.
    using PublishStartHandler = std::function<void(const StreamRegistration&)>;
    // Invoked when a publishing stream is torn down (deleteStream, or the
    // connection closing while publishing — call unregister_all_for_connection
    // separately on the registry and this handler for cleanup at the session
    // owner's level, e.g. a later phase's media pipeline).
    using PublishStopHandler = std::function<void(std::string_view stream_key, std::uint64_t connection_id)>;

    CommandSession(std::uint64_t connection_id, StreamRegistry& registry, StreamKeyValidator key_validator,
                   StreamIdRegistry* stream_id_registry = nullptr);
    // Out-of-line so PlaybackRelay (only forward-declared here) is a
    // complete type where playback_relays_'s unique_ptr destructors run.
    ~CommandSession();
    CommandSession(CommandSession&&) noexcept;
    CommandSession& operator=(CommandSession&&) = delete; // StreamRegistry& member is not reassignable
    CommandSession(const CommandSession&) = delete;
    CommandSession& operator=(const CommandSession&) = delete;

    void set_outgoing_handler(OutgoingHandler handler) { outgoing_handler_ = std::move(handler); }
    void set_publish_start_handler(PublishStartHandler handler) { publish_start_handler_ = std::move(handler); }
    void set_publish_stop_handler(PublishStopHandler handler) { publish_stop_handler_ = std::move(handler); }

    // Injects the Media Ingest component (Phase 5). Optional: if never set,
    // audio/video/metadata messages on a Publishing stream are simply
    // dropped, same as before Phase 5 existed. Not owned — lifetime is the
    // caller's responsibility, matching how `registry_` is a reference, not
    // a value, member.
    void set_media_ingest(media::MediaIngest* media_ingest) { media_ingest_ = media_ingest; }

    // Injects the FLV Recorder (Phase 6). Optional and non-owning, exactly
    // like set_media_ingest: if never set, publishing media is not recorded.
    // Audio/Video/metadata on a Publishing stream are forwarded here (in
    // addition to MediaIngest); the recorder is finalized when the publishing
    // stream is torn down (deleteStream or connection close), including on an
    // abrupt disconnect. See docs/flv-recording.md.
    void set_recorder(RecorderSink* recorder) { recorder_ = recorder; }

    // Injects the Live Fanout hub (Phase 7). Optional and non-owning, same
    // pattern as set_recorder. If never set: `play` still succeeds (status
    // NetStream.Play.Start is sent) but the viewer receives no media, and
    // publishing media is never fanned out — matches the "if never set"
    // no-op convention every other optional collaborator here follows. See
    // docs/rtmp-playback.md.
    void set_live_fanout(LiveFanout* live_fanout) { live_fanout_ = live_fanout; }

    // Reports this connection's current outbound byte backlog (e.g. a
    // TcpConnection's queued-but-unsent bytes). Optional: if never set,
    // playback backpressure treats the backlog as always empty, i.e. never
    // drops. Used only to decide whether to drop a playback frame for a slow
    // viewer (docs/rtmp_promot.md Phase 7 "viewer backpressure"); publishing
    // and command traffic are unaffected.
    using PendingBytesProvider = std::function<std::size_t()>;
    void set_pending_bytes_provider(PendingBytesProvider provider) { pending_bytes_provider_ = std::move(provider); }

    // Byte/packet budget for this connection's playback backlog, folded
    // into pending_bytes_provider_()'s real transport backlog via a
    // ViewerQueue (docs/v2_promot.md PHASE 3 "Implement bounded per-viewer
    // queues" — this replaces the old flat byte-cap check). A playback
    // frame is dropped (not written, PlaybackSink::on_* returns false)
    // whenever the staged policy says to wait for the next keyframe.
    void set_playback_queue_limits(QueueLimits limits) { playback_queue_limits_ = limits; }

    // Feeds one fully-reassembled RTMP message from ChunkDecoder. Amf0Command
    // (type 20) messages are dispatched to the connect/publish/play command
    // handlers below. Audio (8), Video (9), and Amf0Data (18, metadata) are
    // routed to the injected MediaIngest for any stream currently in
    // NetStreamState::Publishing (Phase 5) — see docs/media-ingest.md. Any
    // other message type (protocol control, AMF3, ...) is ignored, see
    // docs/rtmp-commands.md "Known limitations".
    void handle_message(const chunk::RtmpMessage& message);

    // To be called by the connection owner on disconnect, so any stream
    // this connection was publishing is removed from the registry and
    // publish_stop_handler_ fires. Safe to call even if nothing was
    // published.
    void on_connection_closed();

    [[nodiscard]] std::uint64_t connection_id() const noexcept { return connection_id_; }
    [[nodiscard]] bool is_connected() const noexcept { return connected_; }
    [[nodiscard]] const std::string& app_name() const noexcept { return app_name_; }
    [[nodiscard]] NetStreamState stream_state(std::uint32_t stream_id) const;
    [[nodiscard]] std::uint32_t last_created_stream_id() const noexcept { return last_created_stream_id_; }

private:
    class PlaybackRelay; // implements PlaybackSink, forwards into this session

    struct StreamSlot {
        NetStreamState state = NetStreamState::Idle;
        std::string stream_key; // set once publish()/play() names it
        StreamId stream_id;     // resolved at publish()/play() time via stream_id_registry_
    };

    // Called by PlaybackRelay to actually emit an audio/video/metadata
    // message to this connection's outgoing_handler_, applying the
    // backpressure byte-budget check. Returns false (message dropped) if the
    // budget is exceeded or there is no outgoing_handler_.
    bool deliver_playback_message(std::uint32_t message_stream_id, const SharedMediaFrame& frame, ViewerQueue& queue,
                                   bool is_video);
    // PlaybackRelay callbacks: the subscription in live_fanout_ is already
    // gone by the time these run (see PlaybackSink contract).
    void handle_playback_publisher_stopped(std::uint32_t message_stream_id);
    void handle_playback_evicted(std::uint32_t message_stream_id);

    void route_media_message(const chunk::RtmpMessage& message);
    void dispatch(const Amf0Command& command, std::uint32_t message_stream_id);
    void handle_connect(const Amf0Command& command);
    void handle_release_stream(const Amf0Command& command);
    void handle_fc_publish(const Amf0Command& command);
    void handle_create_stream(const Amf0Command& command);
    void handle_publish(const Amf0Command& command, std::uint32_t message_stream_id);
    void handle_play(const Amf0Command& command, std::uint32_t message_stream_id);
    void handle_delete_stream(const Amf0Command& command);

    void send_command_message(std::uint32_t message_stream_id, std::vector<amf0::Amf0Value> values);
    void send_result(double transaction_id, amf0::Amf0Value properties, amf0::Amf0Value information);
    void send_error(double transaction_id, amf0::Amf0Value information);
    void send_status(std::uint32_t message_stream_id, std::string_view level, std::string_view code,
                      std::string_view description);

    std::uint64_t connection_id_;
    StreamRegistry& registry_;
    StreamKeyValidator key_validator_;
    StreamIdRegistry* stream_id_registry_ = nullptr; // never null after construction; see .cpp fallback
    OutgoingHandler outgoing_handler_;
    PublishStartHandler publish_start_handler_;
    PublishStopHandler publish_stop_handler_;
    media::MediaIngest* media_ingest_ = nullptr;
    RecorderSink* recorder_ = nullptr;
    LiveFanout* live_fanout_ = nullptr;
    PendingBytesProvider pending_bytes_provider_;
    QueueLimits playback_queue_limits_;

    bool connected_ = false;
    std::string app_name_;
    std::uint32_t next_stream_id_ = 1;
    std::uint32_t last_created_stream_id_ = 0;
    std::unordered_map<std::uint32_t, StreamSlot> streams_;
    std::unordered_map<std::uint32_t, std::unique_ptr<PlaybackRelay>> playback_relays_;
};

} // namespace rtmp_server::protocol::commands
