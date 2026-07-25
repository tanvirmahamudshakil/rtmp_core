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

std::string string_arg(const Amf0Value& v, std::string fallback = {}) {
    return v.is_string() ? v.as_string() : std::move(fallback);
}

} // namespace

CommandSession::CommandSession(std::uint64_t connection_id, StreamRegistry& registry,
                                 StreamKeyValidator key_validator)
    : connection_id_(connection_id), registry_(registry), key_validator_(std::move(key_validator)) {}

NetStreamState CommandSession::stream_state(std::uint32_t stream_id) const {
    auto it = streams_.find(stream_id);
    return it == streams_.end() ? NetStreamState::Idle : it->second.state;
}

void CommandSession::handle_message(const RtmpMessage& message) {
    if (message.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) {
        // AMF0 Data (18) and everything else is out of scope for command
        // dispatch in this phase (see class doc "Known limitations").
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

    bool registered = registry_.register_publisher(app_name_, stream_key, connection_id_, message_stream_id);
    if (!registered) {
        // Someone else is already publishing this key.
        send_status(message_stream_id, "error", "NetStream.Publish.BadName",
                     "Stream key already in use by another publisher.");
        return;
    }

    auto& slot = streams_[message_stream_id];
    slot.state = NetStreamState::Publishing;
    slot.stream_key = stream_key;

    send_status(message_stream_id, "status", "NetStream.Publish.Start",
                 "Publishing " + stream_key + ".");

    if (publish_start_handler_) {
        auto reg = registry_.find(stream_key);
        if (reg) publish_start_handler_(*reg);
    }
}

void CommandSession::handle_play(const Amf0Command& command, std::uint32_t message_stream_id) {
    std::string stream_key = command.arguments.empty() ? std::string{} : string_arg(command.arguments[0]);

    auto& slot = streams_[message_stream_id];
    slot.state = NetStreamState::Playing;
    slot.stream_key = stream_key;

    send_status(message_stream_id, "status", "NetStream.Play.Start", "Started playing " + stream_key + ".");
}

void CommandSession::handle_delete_stream(const Amf0Command& command) {
    if (command.arguments.empty() || !command.arguments[0].is_number()) return;
    auto stream_id = static_cast<std::uint32_t>(command.arguments[0].as_number());

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    if (it->second.state == NetStreamState::Publishing && !it->second.stream_key.empty()) {
        registry_.unregister_publisher(it->second.stream_key, connection_id_);
        if (publish_stop_handler_) publish_stop_handler_(it->second.stream_key, connection_id_);
    }
    streams_.erase(it);
}

void CommandSession::on_connection_closed() {
    for (auto& [stream_id, slot] : streams_) {
        if (slot.state == NetStreamState::Publishing && !slot.stream_key.empty()) {
            registry_.unregister_publisher(slot.stream_key, connection_id_);
            if (publish_stop_handler_) publish_stop_handler_(slot.stream_key, connection_id_);
        }
    }
    streams_.clear();
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
