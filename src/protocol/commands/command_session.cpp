#include "rtmp_server/protocol/commands/command_session.hpp"

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"

namespace rtmp_server::protocol::commands {

using amf0::Amf0Value;
using chunk::MessageTypeId;
using chunk::RtmpMessage;

namespace {

// Command messages are always sent on chunk stream ID 3 by convention
// (csid 2 is reserved for protocol control) — matches every mainstream RTMP
// server/client. ChunkEncoder itself does not care what csid is used, this
// is purely a session-layer convention.
constexpr std::uint32_t kCommandChunkStreamId = 3;
// Distinct chunk stream IDs for relayed playback media, kept apart from
// kCommandChunkStreamId so ChunkEncoder's per-csid delta-header state
// doesn't mix command traffic with audio/video, matching how real
// encoders/servers keep media on its own chunk streams.
constexpr std::uint32_t kAudioChunkStreamId = 4;
constexpr std::uint32_t kVideoChunkStreamId = 5;
constexpr std::uint32_t kDataChunkStreamId = 6;

std::string string_arg(const Amf0Value& v, std::string fallback = {}) {
    return v.is_string() ? v.as_string() : std::move(fallback);
}

// RTMP User Control Message (type 4) event type. Only StreamBegin (0) is
// produced here; the connection session handles PingRequest/PingResponse.
constexpr std::uint16_t kUserControlStreamBegin = 0;

// Builds a "Stream Begin" User Control Message for `message_stream_id`.
// Every mainstream RTMP client (VLC/librtmp, ffmpeg, Flash) waits for this
// event before it starts rendering the play stream, so it must be sent
// before the NetStream.Play.Start onStatus. The payload is a 2-byte
// big-endian event type followed by the 4-byte big-endian stream ID that
// has become functional. It rides the protocol-control chunk stream
// (csid 2, message stream 0), same as every other User Control Message.
RtmpMessage make_stream_begin(std::uint32_t message_stream_id) {
    RtmpMessage message;
    message.chunk_stream_id = chunk::kProtocolControlChunkStreamId;
    message.message_stream_id = chunk::kProtocolControlMessageStreamId;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::UserControlMessage);
    message.payload.reserve(6);
    message.payload.push_back(static_cast<std::byte>((kUserControlStreamBegin >> 8) & 0xFF));
    message.payload.push_back(static_cast<std::byte>(kUserControlStreamBegin & 0xFF));
    message.payload.push_back(static_cast<std::byte>((message_stream_id >> 24) & 0xFF));
    message.payload.push_back(static_cast<std::byte>((message_stream_id >> 16) & 0xFF));
    message.payload.push_back(static_cast<std::byte>((message_stream_id >> 8) & 0xFF));
    message.payload.push_back(static_cast<std::byte>(message_stream_id & 0xFF));
    return message;
}

} // namespace

namespace {

// Fallback used only when a caller (typically a test, or a component that
// genuinely has no per-server StreamIdRegistry available) does not supply
// one explicitly. A single process-wide instance keeps CommandSession
// itself movable (a member StreamIdRegistry would not be, since it embeds
// a std::mutex) while still giving every CommandSession that doesn't care
// a working, if not globally deduplicated-with-others, registry.
StreamIdRegistry& default_stream_id_registry() {
    static StreamIdRegistry registry;
    return registry;
}

} // namespace

CommandSession::CommandSession(std::uint64_t connection_id, StreamRegistry& registry,
                                 StreamKeyValidator key_validator, StreamIdRegistry* stream_id_registry)
    : connection_id_(connection_id),
      registry_(registry),
      key_validator_(std::move(key_validator)),
      stream_id_registry_(stream_id_registry != nullptr ? stream_id_registry : &default_stream_id_registry()) {}

// Adapts one Playing StreamSlot's subscription to LiveFanout's PlaybackSink
// interface, forwarding every callback back into the owning CommandSession.
// One instance per message_stream_id currently playing, owned by
// playback_relays_; lives exactly as long as the subscription does.
class CommandSession::PlaybackRelay : public PlaybackSink {
public:
    PlaybackRelay(CommandSession& owner, std::uint32_t message_stream_id, QueueLimits limits, SubscriberId subscriber_id)
        : owner_(owner), message_stream_id_(message_stream_id), queue_(limits), subscriber_id_(subscriber_id) {}

    [[nodiscard]] SubscriberId subscriber_id() const noexcept { return subscriber_id_; }

    bool on_audio(const SharedMediaFrame& frame) override {
        return owner_.deliver_playback_message(message_stream_id_, frame, queue_, /*is_video=*/false);
    }
    bool on_video(const SharedMediaFrame& frame) override {
        return owner_.deliver_playback_message(message_stream_id_, frame, queue_, /*is_video=*/true);
    }
    bool on_metadata(const SharedMediaFrame& frame) override {
        return owner_.deliver_playback_message(message_stream_id_, frame, queue_, /*is_video=*/false);
    }
    void on_publisher_stopped() override { owner_.handle_playback_publisher_stopped(message_stream_id_); }
    void on_slow_client_evicted() override { owner_.handle_playback_evicted(message_stream_id_); }

private:
    CommandSession& owner_;
    std::uint32_t message_stream_id_;
    ViewerQueue queue_;
    SubscriberId subscriber_id_;
};

CommandSession::~CommandSession() = default;
CommandSession::CommandSession(CommandSession&&) noexcept = default;

NetStreamState CommandSession::stream_state(std::uint32_t stream_id) const {
    auto it = streams_.find(stream_id);
    return it == streams_.end() ? NetStreamState::Idle : it->second.state;
}

void CommandSession::handle_message(const RtmpMessage& message) {
    if (message.message_type_id == static_cast<std::uint8_t>(MessageTypeId::Audio) ||
        message.message_type_id == static_cast<std::uint8_t>(MessageTypeId::Video) ||
        message.message_type_id == static_cast<std::uint8_t>(MessageTypeId::Amf0Data)) {
        route_media_message(message);
        return;
    }

    if (message.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) {
        // Everything else (protocol control, AMF3, ...) is out of scope for
        // command dispatch in this phase (see class doc "Known limitations").
        return;
    }

    auto decoded = amf0::decode_all(message.payload);
    if (!decoded || decoded.value().empty() || !decoded.value().front().is_string()) {
        // Malformed command message: no name to dispatch on. There is no
        // transaction ID we can trust either, so nothing useful to reply
        // with — silently drop, matching how ChunkDecoder drops bytes past
        // a fail() rather than guessing at recovery.
        return;
    }

    Amf0Command command;
    auto& values = decoded.value();
    command.name = values[0].as_string();
    command.transaction_id = values.size() > 1 && values[1].is_number() ? values[1].as_number() : 0.0;
    command.command_object = values.size() > 2 ? values[2] : Amf0Value::null();
    for (std::size_t i = 3; i < values.size(); ++i) command.arguments.push_back(values[i]);

    dispatch(command, message.message_stream_id);
}

bool CommandSession::deliver_playback_message(std::uint32_t message_stream_id, const SharedMediaFrame& frame,
                                               ViewerQueue& queue, bool is_video) {
    if (!outgoing_handler_) return false;

    bool is_keyframe = false;
    if (is_video) {
        auto info = media::classify_video_tag(frame.payload.view());
        is_keyframe = info && (info->frame_type == media::VideoFrameType::KeyFrame ||
                                info->frame_type == media::VideoFrameType::GeneratedKeyFrame);
    }

    std::size_t pending = pending_bytes_provider_ ? pending_bytes_provider_() : 0;
    auto decision = queue.offer(pending, frame.payload.size(), is_video, is_keyframe);
    // Evict is treated as a drop here rather than a hard disconnect: the
    // authoritative slow-viewer eviction lifecycle lives in LiveFanout
    // (which already forces this subscriber off via on_slow_client_evicted
    // once its own ViewerQueue gives up). This connection-local ViewerQueue
    // only folds the *real* transport backlog (pending_bytes_provider_) into
    // the same staged policy so a viewer whose socket alone is slow (even
    // if LiveFanout's own queue would still accept it) also degrades
    // gracefully instead of an unbounded flat-byte cliff.
    if (decision == ViewerQueue::Decision::DropAndWait || decision == ViewerQueue::Decision::Evict) return false;

    RtmpMessage out = frame.to_message(kCommandChunkStreamId, message_stream_id);
    switch (static_cast<MessageTypeId>(frame.message_type_id)) {
        case MessageTypeId::Audio:
            out.chunk_stream_id = kAudioChunkStreamId;
            break;
        case MessageTypeId::Video:
            out.chunk_stream_id = kVideoChunkStreamId;
            break;
        default:
            out.chunk_stream_id = kDataChunkStreamId;
            break;
    }
    outgoing_handler_(std::move(out));
    queue.note_flushed(frame.payload.size());
    return true;
}

void CommandSession::handle_playback_publisher_stopped(std::uint32_t message_stream_id) {
    auto it = streams_.find(message_stream_id);
    if (it != streams_.end()) {
        if (it->second.state == NetStreamState::Playing && viewer_detached_handler_) {
            viewer_detached_handler_(app_name_, it->second.stream_key);
        }
        it->second.state = NetStreamState::Idle;
        send_status(message_stream_id, "status", "NetStream.Play.UnpublishNotify", "Stream unpublished.");
    }
    playback_relays_.erase(message_stream_id);
}

void CommandSession::handle_playback_evicted(std::uint32_t message_stream_id) {
    auto it = streams_.find(message_stream_id);
    if (it != streams_.end()) {
        if (it->second.state == NetStreamState::Playing && viewer_detached_handler_) {
            viewer_detached_handler_(app_name_, it->second.stream_key);
        }
        it->second.state = NetStreamState::Idle;
        send_status(message_stream_id, "error", "NetStream.Play.InsufficientBW", "Viewer fell too far behind.");
    }
    playback_relays_.erase(message_stream_id);
}

void CommandSession::route_media_message(const RtmpMessage& message) {
    if (media_ingest_ == nullptr && recorder_ == nullptr && live_fanout_ == nullptr) return;

    auto it = streams_.find(message.message_stream_id);
    if (it == streams_.end() || it->second.state != NetStreamState::Publishing || it->second.stream_key.empty()) {
        // Media arriving before publish() completed, or on a non-publishing
        // stream ID, is not this component's concern (Phase 4 already
        // rejects/ignores commands on such streams); silently drop, same
        // policy as the pre-Phase-5 default of dropping type 8/9/18.
        return;
    }

    const std::string& stream_key = it->second.stream_key;
    // One SharedMediaFrame construction per message (one copy out of the
    // decoder's buffer, same cost as constructing RtmpMessage today) shared
    // by value (refcounted, no further copies) with LiveFanout — see
    // shared_media_frame.hpp.
    SharedMediaFrame frame = live_fanout_ != nullptr ? SharedMediaFrame::from_message(message) : SharedMediaFrame{};
    switch (static_cast<MessageTypeId>(message.message_type_id)) {
        case MessageTypeId::Audio:
            if (media_ingest_ != nullptr) media_ingest_->on_audio_message(stream_key, message);
            if (recorder_ != nullptr) recorder_->on_audio(message);
            if (live_fanout_ != nullptr) live_fanout_->on_audio(it->second.stream_id, frame);
            break;
        case MessageTypeId::Video:
            if (media_ingest_ != nullptr) media_ingest_->on_video_message(stream_key, message);
            if (recorder_ != nullptr) recorder_->on_video(message);
            if (live_fanout_ != nullptr) live_fanout_->on_video(it->second.stream_id, frame);
            break;
        case MessageTypeId::Amf0Data:
            if (media_ingest_ != nullptr) media_ingest_->on_metadata_message(stream_key, message);
            if (recorder_ != nullptr) recorder_->on_metadata(message);
            if (live_fanout_ != nullptr) live_fanout_->on_metadata(it->second.stream_id, frame);
            break;
        default:
            break;
    }
}

void CommandSession::dispatch(const Amf0Command& command, std::uint32_t message_stream_id) {
    if (command.name == "connect") {
        handle_connect(command);
    } else if (command.name == "releaseStream") {
        handle_release_stream(command);
    } else if (command.name == "FCPublish") {
        handle_fc_publish(command);
    } else if (command.name == "createStream") {
        handle_create_stream(command);
    } else if (command.name == "publish") {
        handle_publish(command, message_stream_id);
    } else if (command.name == "play") {
        handle_play(command, message_stream_id);
    } else if (command.name == "deleteStream") {
        handle_delete_stream(command);
    }
    // Unrecognized commands (e.g. FCUnpublish, pause, seek — not required by
    // this phase) are silently ignored, matching how real servers tolerate
    // unknown/optional commands rather than erroring the connection.
}

void CommandSession::handle_connect(const Amf0Command& command) {
    const Amf0Value* app = command.command_object.find("app");
    app_name_ = app != nullptr ? string_arg(*app) : std::string{};
    connected_ = true;

    // "Properties" object: server identification. Values match common
    // Adobe Media Server / nginx-rtmp conventions well-understood by OBS.
    Amf0Value properties = Amf0Value::object({
        {"fmsVer", Amf0Value::string("FMS/3,0,1,123")},
        {"capabilities", Amf0Value::number(31)},
    });

    // "Information" object: the actual status. objectEncoding=0 means AMF0
    // (we do not support AMF3 command encoding — message type 17 is
    // explicitly out of scope per the phase spec).
    Amf0Value information = Amf0Value::object({
        {"level", Amf0Value::string("status")},
        {"code", Amf0Value::string("NetConnection.Connect.Success")},
        {"description", Amf0Value::string("Connection succeeded.")},
        {"objectEncoding", Amf0Value::number(0)},
    });

    send_result(command.transaction_id, std::move(properties), std::move(information));
}

void CommandSession::handle_release_stream(const Amf0Command& command) {
    // FMLE/OBS-classic-style pre-publish handshake command. Real encoders
    // do not require a meaningful reply, only *a* _result on the same
    // transaction ID; we have nothing stateful to do since the actual
    // stream key is validated at publish() time.
    send_result(command.transaction_id, Amf0Value::null(), Amf0Value::undefined());
}

void CommandSession::handle_fc_publish(const Amf0Command& command) {
    send_result(command.transaction_id, Amf0Value::null(), Amf0Value::undefined());
}

void CommandSession::handle_create_stream(const Amf0Command& command) {
    std::uint32_t stream_id = next_stream_id_++;
    last_created_stream_id_ = stream_id;
    streams_[stream_id] = StreamSlot{};
    send_result(command.transaction_id, Amf0Value::null(), Amf0Value::number(stream_id));
}

void CommandSession::handle_publish(const Amf0Command& command, std::uint32_t message_stream_id) {
    std::string stream_key = command.arguments.empty() ? std::string{} : string_arg(command.arguments[0]);

    bool authorized = stream_key.empty() ? false : key_validator_(app_name_, stream_key);
    if (!authorized) {
        send_status(message_stream_id, "error", "NetStream.Publish.BadName",
                     "Stream key rejected or missing.");
        return;
    }

    // Phase 5: converge the secret publish key and the public playback name
    // on one fan-out identity. If a resolver is wired, register/fan out
    // under the resolved public name instead of the raw secret key so a
    // stream published with `key` and played back by public `name` are the
    // same internal stream, and the secret key never appears in the live
    // fan-out registry or subscriber-facing state. Without a resolver
    // (unset in every pre-Phase-5 test), behaviour is unchanged.
    std::string canonical_id = stream_key;
    if (stream_id_resolver_) {
        auto resolved = stream_id_resolver_(app_name_, stream_key);
        if (!resolved) {
            send_status(message_stream_id, "error", "NetStream.Publish.BadName",
                         "Stream key rejected or missing.");
            return;
        }
        canonical_id = *resolved;
    }

    bool registered = registry_.register_publisher(app_name_, canonical_id, connection_id_, message_stream_id);
    if (!registered) {
        // Someone else is already publishing this key.
        send_status(message_stream_id, "error", "NetStream.Publish.BadName",
                     "Stream key already in use by another publisher.");
        return;
    }

    auto& slot = streams_[message_stream_id];
    slot.state = NetStreamState::Publishing;
    slot.stream_key = canonical_id;
    slot.stream_id = stream_id_registry_->resolve(app_name_, canonical_id);

    send_status(message_stream_id, "status", "NetStream.Publish.Start",
                 "Publishing " + stream_key + ".");

    if (publish_start_handler_) {
        auto reg = registry_.find(canonical_id);
        if (reg) publish_start_handler_(*reg);
    }
}

void CommandSession::handle_play(const Amf0Command& command, std::uint32_t message_stream_id) {
    std::string raw_arg = command.arguments.empty() ? std::string{} : string_arg(command.arguments[0]);

    // RTMP play names may carry a query string, e.g.
    // "mystream?token=<sig>&expires=<unix>" (see docs/rtmp-playback.md /
    // docs/management-api.md "Playback token"). Split it off before using
    // the name as the fan-out identity — the query string is never part of
    // the stream identity itself.
    std::string stream_key = raw_arg;
    std::string query;
    if (auto qpos = raw_arg.find('?'); qpos != std::string::npos) {
        stream_key = raw_arg.substr(0, qpos);
        query = raw_arg.substr(qpos + 1);
    }

    // Phase 5: gate playback on the injected authorizer (signed playback
    // token validation, stream/app enabled state, viewer/IP limits). Unset
    // in every pre-Phase-5 test, so behaviour there is unchanged.
    if (playback_authorizer_ && !playback_authorizer_(app_name_, stream_key, query, client_ip_)) {
        send_status(message_stream_id, "error", "NetStream.Play.Failed", "Playback not authorized.");
        return;
    }

    auto& slot = streams_[message_stream_id];
    slot.state = NetStreamState::Playing;
    slot.stream_key = stream_key;
    slot.stream_id = stream_id_registry_->resolve(app_name_, stream_key);

    // Signal the stream is now live *before* NetStream.Play.Start and before
    // any cached-GOP media the subscribe() below replays. Without this, VLC/
    // librtmp accepts the connection and the onStatus but never starts
    // rendering — the "publish works, play produces no output" symptom.
    if (outgoing_handler_) outgoing_handler_(make_stream_begin(message_stream_id));

    send_status(message_stream_id, "status", "NetStream.Play.Start", "Started playing " + stream_key + ".");

    if (live_fanout_ != nullptr && !stream_key.empty()) {
        auto subscriber_id = SubscriberId::next();
        auto relay = std::make_unique<PlaybackRelay>(*this, message_stream_id, playback_queue_limits_, subscriber_id);
        playback_relays_[message_stream_id] = std::move(relay);
        live_fanout_->subscribe(slot.stream_id, subscriber_id, playback_relays_[message_stream_id].get());
    }
    if (viewer_attached_handler_) viewer_attached_handler_(app_name_, stream_key);
}

void CommandSession::handle_delete_stream(const Amf0Command& command) {
    if (command.arguments.empty() || !command.arguments[0].is_number()) return;
    auto stream_id = static_cast<std::uint32_t>(command.arguments[0].as_number());

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    if (it->second.state == NetStreamState::Publishing && !it->second.stream_key.empty()) {
        registry_.unregister_publisher(it->second.stream_key, connection_id_);
        if (recorder_ != nullptr) recorder_->finalize();
        if (live_fanout_ != nullptr) live_fanout_->publisher_stopped(it->second.stream_id);
        if (publish_stop_handler_) publish_stop_handler_(it->second.stream_key, connection_id_);
    } else if (it->second.state == NetStreamState::Playing && !it->second.stream_key.empty()) {
        auto relay_it = playback_relays_.find(stream_id);
        if (live_fanout_ != nullptr && relay_it != playback_relays_.end()) {
            live_fanout_->unsubscribe(it->second.stream_id, relay_it->second->subscriber_id());
        }
        playback_relays_.erase(stream_id);
        if (viewer_detached_handler_) viewer_detached_handler_(app_name_, it->second.stream_key);
    }
    streams_.erase(it);
}

void CommandSession::on_connection_closed() {
    for (auto& [stream_id, slot] : streams_) {
        if (slot.state == NetStreamState::Publishing && !slot.stream_key.empty()) {
            registry_.unregister_publisher(slot.stream_key, connection_id_);
            if (recorder_ != nullptr) recorder_->finalize();
            if (live_fanout_ != nullptr) live_fanout_->publisher_stopped(slot.stream_id);
            if (publish_stop_handler_) publish_stop_handler_(slot.stream_key, connection_id_);
        } else if (slot.state == NetStreamState::Playing && !slot.stream_key.empty()) {
            auto relay_it = playback_relays_.find(stream_id);
            if (live_fanout_ != nullptr && relay_it != playback_relays_.end()) {
                live_fanout_->unsubscribe(slot.stream_id, relay_it->second->subscriber_id());
            }
            if (viewer_detached_handler_) viewer_detached_handler_(app_name_, slot.stream_key);
        }
    }
    streams_.clear();
    playback_relays_.clear();
    registry_.unregister_all_for_connection(connection_id_); // belt-and-suspenders
}

void CommandSession::send_command_message(std::uint32_t message_stream_id, std::vector<Amf0Value> values) {
    if (!outgoing_handler_) return;

    RtmpMessage message;
    message.chunk_stream_id = kCommandChunkStreamId;
    message.message_stream_id = message_stream_id;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    message.timestamp = 0;
    for (const auto& value : values) amf0::encode(value, message.payload);

    outgoing_handler_(std::move(message));
}

void CommandSession::send_result(double transaction_id, Amf0Value properties, Amf0Value information) {
    send_command_message(0, {Amf0Value::string("_result"), Amf0Value::number(transaction_id),
                              std::move(properties), std::move(information)});
}

void CommandSession::send_error(double transaction_id, Amf0Value information) {
    send_command_message(0, {Amf0Value::string("_error"), Amf0Value::number(transaction_id), Amf0Value::null(),
                              std::move(information)});
}

void CommandSession::send_status(std::uint32_t message_stream_id, std::string_view level, std::string_view code,
                                   std::string_view description) {
    Amf0Value information = Amf0Value::object({
        {"level", Amf0Value::string(std::string(level))},
        {"code", Amf0Value::string(std::string(code))},
        {"description", Amf0Value::string(std::string(description))},
    });
    send_command_message(message_stream_id, {Amf0Value::string("onStatus"), Amf0Value::number(0),
                                              Amf0Value::null(), std::move(information)});
}

} // namespace rtmp_server::protocol::commands
