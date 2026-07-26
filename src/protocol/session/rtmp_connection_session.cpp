#include "rtmp_server/protocol/session/rtmp_connection_session.hpp"

#include <array>

#include "rtmp_server/core/byte_order.hpp"

namespace rtmp_server::protocol::session {

using chunk::MessageTypeId;
using chunk::RtmpMessage;

namespace {

// RTMP User Control Message (type 4) event types this session must produce/
// consume (Phase 1 task 9). Not modeled in chunk_types.hpp because the
// chunk layer treats User Control Message payloads as opaque (see
// ChunkDecoder::handle_protocol_control's default case).
enum class UserControlEventType : std::uint16_t {
    StreamBegin = 0,
    StreamEof = 1,
    StreamDry = 2,
    SetBufferLength = 3,
    StreamIsRecorded = 4,
    PingRequest = 6,
    PingResponse = 7,
};

void append_u16_be(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(value & 0xFF));
}

void append_u32_be(std::vector<std::byte>& out, std::uint32_t value) {
    std::array<std::byte, 4> tmp{};
    core::write_u32_be(tmp, value);
    out.insert(out.end(), tmp.begin(), tmp.end());
}

RtmpMessage make_user_control(UserControlEventType event, std::uint32_t value) {
    RtmpMessage message;
    message.chunk_stream_id = chunk::kProtocolControlChunkStreamId;
    message.message_stream_id = chunk::kProtocolControlMessageStreamId;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::UserControlMessage);
    append_u16_be(message.payload, static_cast<std::uint16_t>(event));
    append_u32_be(message.payload, value);
    return message;
}

} // namespace

RtmpConnectionSession::RtmpConnectionSession(std::uint64_t connection_id, Dependencies deps,
                                              std::uint32_t max_message_size, std::uint32_t output_chunk_size)
    : connection_id_(connection_id),
      decoder_(max_message_size),
      encoder_(output_chunk_size),
      output_chunk_size_(output_chunk_size),
      command_session_(connection_id, *deps.registry,
                        deps.key_validator ? std::move(deps.key_validator)
                                            : commands::StreamKeyValidator([](std::string_view, std::string_view) {
                                                  return true;
                                              }),
                        deps.stream_id_registry) {
    if (deps.media_ingest != nullptr) command_session_.set_media_ingest(deps.media_ingest);
    if (deps.recorder != nullptr) command_session_.set_recorder(deps.recorder);
    if (deps.live_fanout != nullptr) command_session_.set_live_fanout(deps.live_fanout);
    command_session_.set_playback_queue_limits(deps.playback_queue_limits);

    // Every reply CommandSession decides to send (connect result, publish/
    // play onStatus, playback media, ...) flows out through here, so this is
    // the single point that turns RtmpMessage values into wire bytes
    // (Phase 1 task 7: "Connect ChunkEncoder to real socket output").
    command_session_.set_outgoing_handler([this](RtmpMessage message) { send_encoded(message); });

    decoder_.set_message_handler([this](RtmpMessage message) { handle_decoded_message(std::move(message)); });
    decoder_.set_error_handler([this](core::Error error) { handle_decode_error(error); });
}

void RtmpConnectionSession::start() {
    if (started_) return;
    started_ = true;
    if (output_chunk_size_ != chunk::kDefaultChunkSize) {
        RtmpMessage set_chunk_size;
        set_chunk_size.chunk_stream_id = chunk::kProtocolControlChunkStreamId;
        set_chunk_size.message_stream_id = chunk::kProtocolControlMessageStreamId;
        set_chunk_size.message_type_id = static_cast<std::uint8_t>(MessageTypeId::SetChunkSize);
        append_u32_be(set_chunk_size.payload, output_chunk_size_);
        // Deliberately does NOT go through send_encoded()/encoder_: Set
        // Chunk Size must itself be encoded at the OLD (default) chunk
        // size, exactly like ChunkEncoder::set_chunk_size's own doc note —
        // encoder_.encode_message() below already reflects the new size we
        // just told the peer, correctly, for every message that follows.
        out_scratch_.clear();
        encoder_.encode_message(set_chunk_size, out_scratch_);
        if (outgoing_handler_ && !out_scratch_.empty()) outgoing_handler_(std::move(out_scratch_));
    }
}

void RtmpConnectionSession::set_pending_bytes_provider(std::function<std::size_t()> provider) {
    command_session_.set_pending_bytes_provider(std::move(provider));
}

void RtmpConnectionSession::set_max_queued_playback_bytes(std::size_t bytes) {
    // Preserved for API compatibility: maps onto the new staged ViewerQueue
    // policy's byte cap, leaving its packet cap at the default (see
    // command_session.hpp set_playback_queue_limits / viewer_queue.hpp).
    command_session_.set_playback_queue_limits(commands::QueueLimits{bytes, commands::QueueLimits{}.max_packets});
}

void RtmpConnectionSession::fail(std::string_view /*reason*/) {
    if (failed_) return;
    failed_ = true;
    if (close_handler_) close_handler_();
}

void RtmpConnectionSession::send_encoded(const RtmpMessage& message) {
    if (failed_ || closed_) return;
    out_scratch_.clear();
    encoder_.encode_message(message, out_scratch_);
    if (outgoing_handler_ && !out_scratch_.empty()) outgoing_handler_(std::move(out_scratch_));
}

void RtmpConnectionSession::flush_pending_acknowledgement() {
    if (!decoder_.acknowledgement_due()) return;
    RtmpMessage ack;
    ack.chunk_stream_id = chunk::kProtocolControlChunkStreamId;
    ack.message_stream_id = chunk::kProtocolControlMessageStreamId;
    ack.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Acknowledgement);
    append_u32_be(ack.payload, static_cast<std::uint32_t>(decoder_.bytes_received()));
    send_encoded(ack);
    decoder_.mark_acknowledged();
}

void RtmpConnectionSession::handle_user_control(const RtmpMessage& message) {
    // Reply to a client-initiated Ping Request (event type 6) with a Ping
    // Response (event type 7) carrying the same timestamp value, per the
    // RTMP spec (Phase 1 task 9: "Ping Request / Ping Response"). This
    // session never originates its own PingRequest — liveness here is
    // already covered by the transport-layer idle timeout
    // (IoUringEventLoop::arm_idle_timeout), so an unsolicited keepalive ping
    // is left as a later-phase enhancement rather than invented here.
    if (message.payload.size() < 2) return;
    auto event = static_cast<std::uint16_t>((static_cast<std::uint8_t>(message.payload[0]) << 8) |
                                             static_cast<std::uint8_t>(message.payload[1]));
    if (event != static_cast<std::uint16_t>(UserControlEventType::PingRequest)) return;
    if (message.payload.size() < 6) return;
    std::uint32_t timestamp = core::read_u32_be(std::span<const std::byte, 4>(message.payload.data() + 2, 4));
    send_encoded(make_user_control(UserControlEventType::PingResponse, timestamp));
}

void RtmpConnectionSession::handle_decoded_message(RtmpMessage message) {
    if (failed_) return;

    if (message.message_stream_id == chunk::kProtocolControlMessageStreamId &&
        message.message_type_id == static_cast<std::uint8_t>(MessageTypeId::UserControlMessage)) {
        handle_user_control(message);
        return;
    }
    // Set Peer Bandwidth / Acknowledgement from the peer carry no action
    // this server needs to take beyond having received them (they are
    // informational for the sender's own outbound flow control); Window
    // Acknowledgement Size already updated decoder_ state before reaching
    // here (ChunkDecoder::handle_protocol_control).
    if (message.message_stream_id == chunk::kProtocolControlMessageStreamId &&
        (message.message_type_id == static_cast<std::uint8_t>(MessageTypeId::SetPeerBandwidth) ||
         message.message_type_id == static_cast<std::uint8_t>(MessageTypeId::Acknowledgement))) {
        return;
    }

    command_session_.handle_message(message);
    flush_pending_acknowledgement();
}

void RtmpConnectionSession::handle_decode_error(core::Error /*error*/) {
    // Malformed chunk stream (Phase 1 task 11): ChunkDecoder has already
    // entered its terminal failed() state and will not process further
    // bytes; this session mirrors that and asks the transport to close the
    // connection rather than leaving it half-open.
    fail("chunk decode error");
}

void RtmpConnectionSession::on_bytes_received(std::span<const std::byte> data) {
    if (failed_ || closed_ || data.empty()) return;
    decoder_.on_bytes_received(data);
    if (decoder_.failed() && !failed_) fail("chunk decoder failed");
}

void RtmpConnectionSession::on_connection_closed() {
    if (closed_) return;
    closed_ = true;
    // Deterministic teardown (Phase 1 task 12): removes any publisher/
    // viewer registration this connection held, regardless of whether the
    // disconnect happened mid-handshake-of-a-command, mid-publish, or
    // mid-play (CommandSession::on_connection_closed is itself idempotent
    // and safe to call even if nothing was ever published/played).
    command_session_.on_connection_closed();
}

} // namespace rtmp_server::protocol::session
