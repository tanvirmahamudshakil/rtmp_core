#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtmp_server::protocol::chunk {

// RTMP chunk-stream wire constants (docs/rtmp_promot.md "RTMP Chunk
// Protocol", docs/chunk-parser.md has the full write-up). All chunk streams
// share these limits regardless of chunk stream ID.
inline constexpr std::uint32_t kDefaultChunkSize = 128;
inline constexpr std::uint32_t kExtendedTimestampMarker = 0x00FFFFFF;
inline constexpr std::uint32_t kMinChunkStreamId = 2; // 0 and 1 are basic-header escape markers
inline constexpr std::uint32_t kProtocolControlChunkStreamId = 2;
inline constexpr std::uint32_t kProtocolControlMessageStreamId = 0;

// Message type IDs relevant to the chunk/protocol-control layer. Media
// (audio=8/video=9), AMF0 command/data (20/18) etc. are opaque payloads as
// far as the chunk engine is concerned and are simply handed to the caller.
enum class MessageTypeId : std::uint8_t {
    SetChunkSize = 1,
    AbortMessage = 2,
    Acknowledgement = 3,
    UserControlMessage = 4,
    WindowAcknowledgementSize = 5,
    SetPeerBandwidth = 6,
    Audio = 8,
    Video = 9,
    Amf3Data = 15,
    Amf3SharedObject = 16,
    Amf3Command = 17,
    Amf0Data = 18,
    Amf0SharedObject = 19,
    Amf0Command = 20,
    Aggregate = 22,
};

// "Limit type" byte carried by Set Peer Bandwidth (docs/rtmp_promot.md).
enum class PeerBandwidthLimitType : std::uint8_t {
    Hard = 0,
    Soft = 1,
    Dynamic = 2,
};

// One fully reassembled RTMP message: the payload of every chunk belonging
// to it, concatenated in order, plus the message header fields that were in
// effect when reassembly completed. Produced by ChunkDecoder, consumed by
// ChunkEncoder (round-trippable).
struct RtmpMessage {
    std::uint32_t chunk_stream_id = 0;
    std::uint32_t message_stream_id = 0;
    std::uint8_t message_type_id = 0;
    std::uint32_t timestamp = 0; // absolute, wraps at 2^32 per RTMP spec
    std::vector<std::byte> payload;
};

} // namespace rtmp_server::protocol::chunk
