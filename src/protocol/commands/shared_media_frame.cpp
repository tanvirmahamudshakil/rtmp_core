#include "rtmp_server/protocol/commands/shared_media_frame.hpp"

#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"

namespace rtmp_server::protocol::commands {

core::SharedBuffer SharedMediaFrame::wire_bytes(std::uint32_t chunk_size,
                                                std::uint32_t chunk_stream_id,
                                                std::uint32_t message_stream_id) const {
    // One entry covers normal clients (their first createStream returns 1);
    // a second covers the occasional connection playing on another message
    // stream. More variants are encoded on demand but not retained, keeping
    // a GOP cache from multiplying its memory footprint under adversarial
    // message-stream IDs.
    constexpr std::size_t kMaximumCachedEncodings = 2;
    std::lock_guard lock(wire_cache_->mutex);

    for (const auto& entry : wire_cache_->entries) {
        if (entry.chunk_size == chunk_size &&
            entry.chunk_stream_id == chunk_stream_id &&
            entry.message_stream_id == message_stream_id) {
            return entry.bytes;
        }
    }

    std::vector<std::byte> encoded;
    // A conservative reserve avoids repeated growth for the common case.
    // Continuation headers can add a little more than payload.size().
    encoded.reserve(payload.size() + 64);
    chunk::ChunkEncoder::encode_message_fmt0(chunk_size, chunk_stream_id,
                                             message_stream_id, message_type_id,
                                             timestamp, payload.view(), encoded);
    auto shared = core::SharedBuffer::adopt(std::move(encoded));
    if (wire_cache_->entries.size() < kMaximumCachedEncodings) {
        wire_cache_->entries.push_back(
            WireEncoding{chunk_size, chunk_stream_id, message_stream_id, shared});
    }
    return shared;
}

} // namespace rtmp_server::protocol::commands
