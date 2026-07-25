#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"

#include <algorithm>
#include <array>
#include <cassert>

#include "rtmp_server/core/byte_order.hpp"

namespace rtmp_server::protocol::chunk {

namespace {

void append_u24_be(std::vector<std::byte>& out, std::uint32_t value) {
    std::array<std::byte, 3> tmp{};
    core::write_u24_be(tmp, value);
    out.insert(out.end(), tmp.begin(), tmp.end());
}

void append_u32_be(std::vector<std::byte>& out, std::uint32_t value) {
    std::array<std::byte, 4> tmp{};
    core::write_u32_be(tmp, value);
    out.insert(out.end(), tmp.begin(), tmp.end());
}

// Message stream IDs are the one field the RTMP spec encodes little-endian.
void append_u32_le(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
}

} // namespace

ChunkEncoder::ChunkEncoder(std::uint32_t chunk_size) : chunk_size_(chunk_size) {
    assert(chunk_size_ > 0);
}

void ChunkEncoder::set_chunk_size(std::uint32_t chunk_size) {
    if (chunk_size == 0) return; // spec forbids zero; ignore rather than corrupt state
    chunk_size_ = chunk_size;
}

void ChunkEncoder::write_basic_header(std::vector<std::byte>& out, std::uint8_t fmt, std::uint32_t csid) {
    std::uint8_t fmt_bits = static_cast<std::uint8_t>((fmt & 0x03) << 6);
    if (csid < 64) {
        out.push_back(static_cast<std::byte>(fmt_bits | static_cast<std::uint8_t>(csid)));
    } else if (csid < 64 + 256) {
        out.push_back(static_cast<std::byte>(fmt_bits)); // csid field = 0
        out.push_back(static_cast<std::byte>(csid - 64));
    } else {
        out.push_back(static_cast<std::byte>(fmt_bits | 0x01)); // csid field = 1
        std::uint32_t remainder = csid - 64;
        out.push_back(static_cast<std::byte>(remainder & 0xFF));
        out.push_back(static_cast<std::byte>((remainder >> 8) & 0xFF));
    }
}

void ChunkEncoder::encode_message(const RtmpMessage& message, std::vector<std::byte>& out) {
    ChunkStreamState& state = streams_[message.chunk_stream_id];

    std::uint32_t delta = 0;
    std::uint8_t fmt = 0;
    bool have_valid_delta = state.has_header && message.timestamp >= state.timestamp;

    if (!state.has_header || message.message_stream_id != state.message_stream_id || !have_valid_delta) {
        fmt = 0;
    } else {
        delta = message.timestamp - state.timestamp;
        if (message.payload.size() != state.message_length || message.message_type_id != state.message_type_id) {
            fmt = 1;
        } else if (delta != state.timestamp_delta) {
            fmt = 2;
        } else {
            fmt = 3;
        }
    }

    bool extended_active;
    std::uint32_t extended_value = 0;
    if (fmt == 0) {
        extended_active = message.timestamp >= kExtendedTimestampMarker;
        extended_value = message.timestamp;
    } else if (fmt == 1 || fmt == 2) {
        extended_active = delta >= kExtendedTimestampMarker;
        extended_value = delta;
    } else {
        // fmt3: repeats the previously established extended-timestamp state
        // verbatim (spec requirement) rather than re-deriving it.
        extended_active = state.extended_timestamp;
        extended_value = state.extended_value;
    }

    write_basic_header(out, fmt, message.chunk_stream_id);
    if (fmt == 0) {
        append_u24_be(out, extended_active ? kExtendedTimestampMarker : message.timestamp);
        append_u24_be(out, static_cast<std::uint32_t>(message.payload.size()));
        out.push_back(static_cast<std::byte>(message.message_type_id));
        append_u32_le(out, message.message_stream_id);
    } else if (fmt == 1) {
        append_u24_be(out, extended_active ? kExtendedTimestampMarker : delta);
        append_u24_be(out, static_cast<std::uint32_t>(message.payload.size()));
        out.push_back(static_cast<std::byte>(message.message_type_id));
    } else if (fmt == 2) {
        append_u24_be(out, extended_active ? kExtendedTimestampMarker : delta);
    }
    if (extended_active) append_u32_be(out, extended_value);

    state.has_header = true;
    state.timestamp = message.timestamp;
    state.timestamp_delta = (fmt == 0) ? 0 : delta;
    state.message_length = static_cast<std::uint32_t>(message.payload.size());
    state.message_type_id = message.message_type_id;
    state.message_stream_id = message.message_stream_id;
    state.extended_timestamp = extended_active;
    state.extended_value = extended_value;

    // Payload, split into chunk_size_-byte pieces; every piece after the
    // first is prefixed with a fmt3 basic header (continuation of the same
    // message), repeating the extended timestamp field if active.
    std::size_t offset = 0;
    std::size_t total = message.payload.size();
    bool first_piece = true;
    do {
        std::size_t piece = std::min<std::size_t>(chunk_size_, total - offset);
        if (!first_piece) {
            write_basic_header(out, 3, message.chunk_stream_id);
            if (extended_active) append_u32_be(out, extended_value);
        }
        out.insert(out.end(), message.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                   message.payload.begin() + static_cast<std::ptrdiff_t>(offset + piece));
        offset += piece;
        first_piece = false;
    } while (offset < total);
}

void ChunkEncoder::encode_set_chunk_size(std::uint32_t new_chunk_size, std::vector<std::byte>& out) {
    RtmpMessage message;
    message.chunk_stream_id = kProtocolControlChunkStreamId;
    message.message_stream_id = kProtocolControlMessageStreamId;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::SetChunkSize);
    message.payload.resize(4);
    core::write_u32_be(std::span<std::byte, 4>(message.payload), new_chunk_size & 0x7FFFFFFF);
    encode_message(message, out);
}

void ChunkEncoder::encode_abort_message(std::uint32_t target_chunk_stream_id, std::vector<std::byte>& out) {
    RtmpMessage message;
    message.chunk_stream_id = kProtocolControlChunkStreamId;
    message.message_stream_id = kProtocolControlMessageStreamId;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::AbortMessage);
    message.payload.resize(4);
    core::write_u32_be(std::span<std::byte, 4>(message.payload), target_chunk_stream_id);
    encode_message(message, out);
}

void ChunkEncoder::encode_acknowledgement(std::uint32_t sequence_number, std::vector<std::byte>& out) {
    RtmpMessage message;
    message.chunk_stream_id = kProtocolControlChunkStreamId;
    message.message_stream_id = kProtocolControlMessageStreamId;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Acknowledgement);
    message.payload.resize(4);
    core::write_u32_be(std::span<std::byte, 4>(message.payload), sequence_number);
    encode_message(message, out);
}

void ChunkEncoder::encode_window_acknowledgement_size(std::uint32_t window_size, std::vector<std::byte>& out) {
    RtmpMessage message;
    message.chunk_stream_id = kProtocolControlChunkStreamId;
    message.message_stream_id = kProtocolControlMessageStreamId;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::WindowAcknowledgementSize);
    message.payload.resize(4);
    core::write_u32_be(std::span<std::byte, 4>(message.payload), window_size);
    encode_message(message, out);
}

void ChunkEncoder::encode_set_peer_bandwidth(std::uint32_t window_size, PeerBandwidthLimitType limit_type,
                                              std::vector<std::byte>& out) {
    RtmpMessage message;
    message.chunk_stream_id = kProtocolControlChunkStreamId;
    message.message_stream_id = kProtocolControlMessageStreamId;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::SetPeerBandwidth);
    message.payload.resize(5);
    core::write_u32_be(std::span<std::byte, 4>(message.payload.data(), 4), window_size);
    message.payload[4] = static_cast<std::byte>(limit_type);
    encode_message(message, out);
}

} // namespace rtmp_server::protocol::chunk
