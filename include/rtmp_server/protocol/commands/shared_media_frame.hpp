#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

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

    // Returns a stateless fmt0 RTMP wire representation. Copies of this
    // SharedMediaFrame share the cache, including copies placed in a GOP
    // cache or forwarded to another io_uring worker. The small bounded key
    // set covers the normal one-message-stream case without allowing an
    // unusual client that creates many RTMP message streams to grow a
    // retained frame without limit.
    [[nodiscard]] core::SharedBuffer wire_bytes(std::uint32_t chunk_size,
                                                std::uint32_t chunk_stream_id,
                                                std::uint32_t message_stream_id) const;

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

private:
    struct WireEncoding {
        std::uint32_t chunk_size = 0;
        std::uint32_t chunk_stream_id = 0;
        std::uint32_t message_stream_id = 0;
        core::SharedBuffer bytes;
    };
    struct WireCache {
        std::mutex mutex;
        std::vector<WireEncoding> entries;
    };

    // shared_ptr preserves cache identity when a frame is copied into the
    // GOP cache, subscriber delivery list, or a cross-worker queue.
    mutable std::shared_ptr<WireCache> wire_cache_ = std::make_shared<WireCache>();
};

} // namespace rtmp_server::protocol::commands
