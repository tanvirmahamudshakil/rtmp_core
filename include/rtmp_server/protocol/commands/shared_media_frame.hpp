#pragma once

#include <cstdint>

#include "rtmp_server/core/buffer.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"

namespace rtmp_server::protocol::commands {

// Immutable, reference-counted stand-in for chunk::RtmpMessage used
// wherever a media payload is shared (GOP cache, N subscriber queues)
// instead of value-copied. Deliberately does not replace RtmpMessage's
// shape everywhere (too many call sites: ChunkDecoder/ChunkEncoder,
// RecorderSink, MediaIngest all take/produce RtmpMessage by value) — this
// is the fan-out-path-only type; conversion happens exactly twice per
// message: once in (from_message, at ingest) and once per delivered
// subscriber at the point it must become wire bytes again (to_message).
struct SharedMediaFrame {
    core::SharedBuffer payload;
    std::uint8_t message_type_id = 0; // chunk::MessageTypeId
    std::uint32_t timestamp = 0;

    static SharedMediaFrame from_message(const chunk::RtmpMessage& message) {
        SharedMediaFrame frame;
        frame.payload = core::SharedBuffer::copy_from(message.payload);
        frame.message_type_id = message.message_type_id;
        frame.timestamp = message.timestamp;
        return frame;
    }

    [[nodiscard]] chunk::RtmpMessage to_message(std::uint32_t chunk_stream_id, std::uint32_t message_stream_id) const {
        chunk::RtmpMessage message;
        message.chunk_stream_id = chunk_stream_id;
        message.message_stream_id = message_stream_id;
        message.message_type_id = message_type_id;
        message.timestamp = timestamp;
        auto view = payload.view();
        message.payload.assign(view.begin(), view.end());
        return message;
    }
};

} // namespace rtmp_server::protocol::commands
