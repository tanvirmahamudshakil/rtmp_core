#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "rtmp_server/protocol/amf0/amf0_value.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"

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

    CommandSession(std::uint64_t connection_id, StreamRegistry& registry, StreamKeyValidator key_validator);

    void set_outgoing_handler(OutgoingHandler handler) { outgoing_handler_ = std::move(handler); }
    void set_publish_start_handler(PublishStartHandler handler) { publish_start_handler_ = std::move(handler); }
    void set_publish_stop_handler(PublishStopHandler handler) { publish_stop_handler_ = std::move(handler); }

    // Feeds one fully-reassembled RTMP message from ChunkDecoder. Messages
    // other than Amf0Command (type 20) are ignored by this phase (AMF0 Data
    // / media / protocol-control messages are handled by ChunkDecoder or
    // later phases) — see docs/rtmp-commands.md "Known limitations".
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
    struct StreamSlot {
        NetStreamState state = NetStreamState::Idle;
        std::string stream_key; // set once publish()/play() names it
    };

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
    OutgoingHandler outgoing_handler_;
    PublishStartHandler publish_start_handler_;
    PublishStopHandler publish_stop_handler_;

    bool connected_ = false;
    std::string app_name_;
    std::uint32_t next_stream_id_ = 1;
    std::uint32_t last_created_stream_id_ = 0;
    std::unordered_map<std::uint32_t, StreamSlot> streams_;
};

} // namespace rtmp_server::protocol::commands
