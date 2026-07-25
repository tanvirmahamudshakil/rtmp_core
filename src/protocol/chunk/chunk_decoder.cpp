#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"

#include <algorithm>
#include <array>

#include "rtmp_server/core/byte_order.hpp"

namespace rtmp_server::protocol::chunk {

namespace {

std::uint32_t read_u24_be_at(std::span<const std::byte> buf, std::size_t offset) {
    return core::read_u24_be(std::span<const std::byte, 3>(buf.data() + offset, 3));
}

std::uint32_t read_u32_be_at(std::span<const std::byte> buf, std::size_t offset) {
    return core::read_u32_be(std::span<const std::byte, 4>(buf.data() + offset, 4));
}

// RTMP message stream IDs inside the fmt-0 chunk message header are encoded
// little-endian (the one field in the whole protocol that is), per the RTMP
// specification.
std::uint32_t read_u32_le_at(std::span<const std::byte> buf, std::size_t offset) {
    return (static_cast<std::uint32_t>(buf[offset])) |
           (static_cast<std::uint32_t>(buf[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(buf[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(buf[offset + 3]) << 24);
}

} // namespace

ChunkDecoder::ChunkDecoder(std::uint32_t max_message_size) : max_message_size_(max_message_size) {}

void ChunkDecoder::fail(core::ErrorCode code, std::string_view message) {
    if (failed_) return;
    failed_ = true;
    buffer_.clear();
    streams_.clear();
    if (error_handler_) error_handler_(core::Error(code, core::ErrorCategory::Protocol, message));
}

bool ChunkDecoder::acknowledgement_due() const noexcept {
    if (window_ack_size_ == 0) return false;
    return bytes_received_ - bytes_at_last_ack_ >= window_ack_size_;
}

void ChunkDecoder::on_bytes_received(std::span<const std::byte> data) {
    if (failed_ || data.empty()) return;

    bytes_received_ += data.size();
    buffer_.insert(buffer_.end(), data.begin(), data.end());

    for (;;) {
        DecodeResult result = decode_one();
        if (result == DecodeResult::InsufficientData) break;
        if (result == DecodeResult::Failed) break; // fail() already invoked the handler
    }
}

ChunkDecoder::DecodeResult ChunkDecoder::decode_one() {
    if (buffer_.empty()) return DecodeResult::InsufficientData;

    // --- Basic Header (1, 2, or 3 bytes) ---
    auto first = static_cast<std::uint8_t>(buffer_[0]);
    std::uint8_t fmt = (first >> 6) & 0x03;
    std::uint8_t csid_field = first & 0x3F;

    std::size_t basic_header_len = 1;
    if (csid_field == 0) {
        basic_header_len = 2;
    } else if (csid_field == 1) {
        basic_header_len = 3;
    }
    if (buffer_.size() < basic_header_len) return DecodeResult::InsufficientData;

    std::uint32_t csid = csid_field;
    if (csid_field == 0) {
        csid = 64 + static_cast<std::uint8_t>(buffer_[1]);
    } else if (csid_field == 1) {
        csid = 64 + static_cast<std::uint8_t>(buffer_[1]) +
               static_cast<std::uint32_t>(static_cast<std::uint8_t>(buffer_[2])) * 256;
    }

    // --- Message Header (11 / 7 / 3 / 0 bytes) ---
    std::size_t message_header_len = 0;
    switch (fmt) {
        case 0: message_header_len = 11; break;
        case 1: message_header_len = 7; break;
        case 2: message_header_len = 3; break;
        case 3: message_header_len = 0; break;
        default: break;
    }
    if (buffer_.size() < basic_header_len + message_header_len) return DecodeResult::InsufficientData;

    auto it = streams_.find(csid);
    bool is_new_stream = (it == streams_.end());
    if (fmt != 0 && is_new_stream) {
        fail(core::ErrorCode::MalformedChunk, "first chunk on a chunk stream ID must use fmt 0");
        return DecodeResult::Failed;
    }
    ChunkStreamState& state = is_new_stream ? streams_[csid] : it->second;

    std::size_t offset = basic_header_len;
    std::uint32_t raw_ts_field = 0;   // 24-bit field as read (timestamp for fmt0, delta for fmt1/2)
    std::uint32_t header_message_length = state.message_length;
    std::uint8_t header_message_type_id = state.message_type_id;
    std::uint32_t header_message_stream_id = state.message_stream_id;

    bool is_continuation = state.has_header && state.bytes_remaining > 0;
    if (fmt != 3 && is_continuation) {
        fail(core::ErrorCode::MalformedChunk,
             "non-fmt3 chunk received while a message on this chunk stream ID is still in progress");
        return DecodeResult::Failed;
    }

    if (fmt == 0) {
        raw_ts_field = read_u24_be_at(buffer_, offset);
        header_message_length = read_u24_be_at(buffer_, offset + 3);
        header_message_type_id = static_cast<std::uint8_t>(buffer_[offset + 6]);
        header_message_stream_id = read_u32_le_at(buffer_, offset + 7);
    } else if (fmt == 1) {
        raw_ts_field = read_u24_be_at(buffer_, offset);
        header_message_length = read_u24_be_at(buffer_, offset + 3);
        header_message_type_id = static_cast<std::uint8_t>(buffer_[offset + 6]);
    } else if (fmt == 2) {
        raw_ts_field = read_u24_be_at(buffer_, offset);
    }
    offset += message_header_len;

    bool needs_extended = false;
    if (fmt == 3) {
        // Per the RTMP spec, once a chunk stream's timestamp/delta requires
        // the extended timestamp field, every subsequent chunk on that
        // stream — including fmt3 chunks — repeats the 4-byte field for as
        // long as that condition holds.
        needs_extended = state.extended_timestamp;
    } else {
        needs_extended = (raw_ts_field == kExtendedTimestampMarker);
    }

    std::size_t total_header_len = offset;
    if (needs_extended) total_header_len += 4;
    if (buffer_.size() < total_header_len) return DecodeResult::InsufficientData;

    std::uint32_t extended_value = 0;
    if (needs_extended) extended_value = read_u32_be_at(buffer_, offset);

    // --- Resolve this chunk's effective header fields without mutating any
    // persisted state yet — we may still discover there isn't enough buffer
    // for the payload slice below, in which case nothing may be committed.
    std::uint32_t effective_length = header_message_length;
    std::uint8_t effective_type_id = header_message_type_id;
    std::uint32_t effective_stream_id = (fmt == 0) ? header_message_stream_id : state.message_stream_id;
    std::uint32_t effective_delta = state.timestamp_delta;
    std::uint32_t effective_timestamp = state.timestamp;
    bool starting_new_message = !is_continuation;

    if (starting_new_message) {
        if (fmt == 0) {
            effective_timestamp = needs_extended ? extended_value : raw_ts_field;
            effective_delta = 0;
        } else if (fmt == 3) {
            // fmt3 starting a *new* message (as opposed to continuing an
            // in-progress one) carries no delta field of its own: the RTMP
            // spec has it reuse the most recently seen delta on this chunk
            // stream ID, unless the extended-timestamp field is active, in
            // which case that field is resent and used verbatim.
            effective_delta = needs_extended ? extended_value : state.timestamp_delta;
            effective_timestamp = state.timestamp + effective_delta;
        } else {
            effective_delta = needs_extended ? extended_value : raw_ts_field;
            effective_timestamp = state.timestamp + effective_delta;
        }
        if (effective_length > max_message_size_) {
            fail(core::ErrorCode::MessageTooLarge, "RTMP message length exceeds the configured maximum");
            return DecodeResult::Failed;
        }
    }

    std::uint32_t bytes_remaining = starting_new_message ? effective_length : state.bytes_remaining;
    std::uint32_t payload_slice = std::min(bytes_remaining, input_chunk_size_);
    // A message with a declared length of 0 (e.g. some degenerate control
    // messages) still needs exactly one chunk to "complete" it.
    std::size_t total_needed = total_header_len + payload_slice;
    if (buffer_.size() < total_needed) return DecodeResult::InsufficientData;

    // --- Commit: consume header bytes, then payload bytes. ---
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total_header_len));

    if (starting_new_message) {
        state.has_header = true;
        state.message_length = effective_length;
        state.message_type_id = effective_type_id;
        state.message_stream_id = effective_stream_id;
        state.timestamp = effective_timestamp;
        state.timestamp_delta = effective_delta;
        state.extended_timestamp = needs_extended;
        state.message_timestamp = effective_timestamp;
        state.partial_payload.clear();
        state.partial_payload.reserve(effective_length);
        state.bytes_remaining = effective_length;
    }

    if (payload_slice > 0) {
        state.partial_payload.insert(state.partial_payload.end(), buffer_.begin(),
                                      buffer_.begin() + static_cast<std::ptrdiff_t>(payload_slice));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(payload_slice));
        state.bytes_remaining -= payload_slice;
    }

    if (state.bytes_remaining == 0) {
        handle_complete_message(csid, state);
    }

    return DecodeResult::Consumed;
}

void ChunkDecoder::handle_complete_message(std::uint32_t csid, ChunkStreamState& state) {
    RtmpMessage message;
    message.chunk_stream_id = csid;
    message.message_stream_id = state.message_stream_id;
    message.message_type_id = state.message_type_id;
    message.timestamp = state.message_timestamp;
    message.payload = std::move(state.partial_payload);
    state.partial_payload.clear();

    if (handle_protocol_control(message)) return;
    if (message_handler_) message_handler_(std::move(message));
}

bool ChunkDecoder::handle_protocol_control(const RtmpMessage& message) {
    if (message.message_stream_id != kProtocolControlMessageStreamId) return false;

    switch (static_cast<MessageTypeId>(message.message_type_id)) {
        case MessageTypeId::SetChunkSize: {
            if (message.payload.size() < 4) {
                fail(core::ErrorCode::MalformedChunk, "Set Chunk Size payload must be 4 bytes");
                return true;
            }
            std::uint32_t value = read_u32_be_at(message.payload, 0) & 0x7FFFFFFF;
            if (value == 0) {
                fail(core::ErrorCode::MalformedChunk, "Set Chunk Size must not be zero");
                return true;
            }
            input_chunk_size_ = value;
            return true;
        }
        case MessageTypeId::AbortMessage: {
            if (message.payload.size() < 4) {
                fail(core::ErrorCode::MalformedChunk, "Abort Message payload must be 4 bytes");
                return true;
            }
            std::uint32_t target_csid = read_u32_be_at(message.payload, 0);
            auto it = streams_.find(target_csid);
            if (it != streams_.end()) {
                it->second.partial_payload.clear();
                it->second.bytes_remaining = 0;
            }
            return true;
        }
        case MessageTypeId::WindowAcknowledgementSize: {
            if (message.payload.size() < 4) {
                fail(core::ErrorCode::MalformedChunk, "Window Acknowledgement Size payload must be 4 bytes");
                return true;
            }
            window_ack_size_ = read_u32_be_at(message.payload, 0);
            return false; // still delivered to the caller: session-level logic may care too
        }
        default:
            return false; // Acknowledgement, Set Peer Bandwidth, User Control, media, AMF, etc.
    }
}

} // namespace rtmp_server::protocol::chunk
