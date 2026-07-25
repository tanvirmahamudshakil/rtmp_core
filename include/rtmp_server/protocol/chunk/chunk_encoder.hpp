#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rtmp_server/protocol/chunk/chunk_types.hpp"

namespace rtmp_server::protocol::chunk {

// Serializes outgoing RTMP messages into chunks (docs/chunk-parser.md).
// Stateful per connection: tracks the last header written per chunk stream
// ID so it can pick the smallest correct message-header form (fmt 0-3,
// mirroring ChunkDecoder's inheritance rules) rather than always emitting
// fmt0. Pure protocol logic — produces byte vectors, does not touch a
// socket or io_uring.
class ChunkEncoder {
public:
    explicit ChunkEncoder(std::uint32_t chunk_size = kDefaultChunkSize);

    // Changes the chunk size used for subsequently encoded messages. Does
    // NOT emit the Set Chunk Size protocol-control message itself — callers
    // that need the peer informed must also encode and send one (see
    // encode_set_chunk_size), typically in the same call that changes this.
    void set_chunk_size(std::uint32_t chunk_size);
    [[nodiscard]] std::uint32_t chunk_size() const noexcept { return chunk_size_; }

    // Appends the chunk-encoded form of `message` to `out`, choosing the
    // smallest valid basic/message header form given this chunk stream ID's
    // prior header (fmt 0-3 inheritance), splitting the payload into
    // chunk_size()-sized pieces (fmt3-prefixed continuations), and emitting
    // the extended timestamp field wherever the timestamp/delta requires it.
    void encode_message(const RtmpMessage& message, std::vector<std::byte>& out);

    // Convenience helpers for the five protocol-control messages, always
    // sent on chunk stream ID 2 / message stream ID 0 per spec. Each both
    // builds the RtmpMessage and encodes it via encode_message().
    void encode_set_chunk_size(std::uint32_t new_chunk_size, std::vector<std::byte>& out);
    void encode_abort_message(std::uint32_t target_chunk_stream_id, std::vector<std::byte>& out);
    void encode_acknowledgement(std::uint32_t sequence_number, std::vector<std::byte>& out);
    void encode_window_acknowledgement_size(std::uint32_t window_size, std::vector<std::byte>& out);
    void encode_set_peer_bandwidth(std::uint32_t window_size, PeerBandwidthLimitType limit_type,
                                    std::vector<std::byte>& out);

private:
    struct ChunkStreamState {
        bool has_header = false;
        std::uint32_t timestamp = 0;
        std::uint32_t timestamp_delta = 0;
        std::uint32_t message_length = 0;
        std::uint8_t message_type_id = 0;
        std::uint32_t message_stream_id = 0;
        bool extended_timestamp = false;
        std::uint32_t extended_value = 0;
    };

    static void write_basic_header(std::vector<std::byte>& out, std::uint8_t fmt, std::uint32_t csid);

    std::uint32_t chunk_size_;
    std::unordered_map<std::uint32_t, ChunkStreamState> streams_;
};

} // namespace rtmp_server::protocol::chunk
