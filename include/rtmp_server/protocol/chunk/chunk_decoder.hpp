#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"

namespace rtmp_server::protocol::chunk {

// Stateful, per-connection RTMP chunk demuxer (docs/chunk-parser.md has the
// full write-up). Pure protocol logic: no sockets, no io_uring — driven
// purely by byte spans handed in via on_bytes_received(), which may be
// fragmented arbitrarily by the transport layer, exactly like
// protocol::handshake::HandshakeSession (docs/architecture.md "Architectural
// Separation").
//
// Handles Basic Header forms (1/2/3 byte), Message Header types 0-3 with
// per-chunk-stream-ID header inheritance, extended timestamps, interleaved
// chunk streams, and the subset of protocol-control messages that affect
// decoding directly (Set Chunk Size, Abort Message). Window Acknowledgement
// Size updates the acknowledgement-window used by bytes_received()-based
// tracking. Acknowledgement and Set Peer Bandwidth carry no decoder-side
// state and are simply delivered to the message handler like any other
// message, since acting on them is a session-level (not chunk-level)
// concern.
class ChunkDecoder {
public:
    using MessageHandler = std::function<void(RtmpMessage)>;
    using ErrorHandler = std::function<void(core::Error)>;

    explicit ChunkDecoder(std::uint32_t max_message_size);

    void set_message_handler(MessageHandler handler) { message_handler_ = std::move(handler); }
    void set_error_handler(ErrorHandler handler) { error_handler_ = std::move(handler); }

    // Feeds a fragment of bytes read off the wire, in order. May be called
    // any number of times with any chunk sizes, including a single byte at a
    // time or multiple chunks'/messages' worth at once. Invokes the message
    // handler synchronously for every message that becomes complete as a
    // result of this call, in wire order. No-op once failed().
    void on_bytes_received(std::span<const std::byte> data);

    // Current input chunk size (payload bytes per chunk expected from the
    // peer), as last set by a Set Chunk Size protocol-control message, or
    // kDefaultChunkSize if none has been received yet.
    [[nodiscard]] std::uint32_t input_chunk_size() const noexcept { return input_chunk_size_; }

    // Total number of raw bytes handed to on_bytes_received() so far.
    [[nodiscard]] std::uint64_t bytes_received() const noexcept { return bytes_received_; }

    // Window Acknowledgement Size in effect (0 = none received; no
    // acknowledgement is ever due). Updated automatically when a Window
    // Acknowledgement Size protocol-control message is decoded.
    [[nodiscard]] std::uint32_t window_acknowledgement_size() const noexcept { return window_ack_size_; }

    // True once bytes_received() has advanced by at least
    // window_acknowledgement_size() since the last mark_acknowledged() call.
    // The caller (session layer) is expected to send an Acknowledgement
    // message (via ChunkEncoder::encode_acknowledgement) and then call
    // mark_acknowledged().
    [[nodiscard]] bool acknowledgement_due() const noexcept;
    void mark_acknowledged() noexcept { bytes_at_last_ack_ = bytes_received_; }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    // Per-chunk-stream-ID header/reassembly state, carried across calls so
    // fmt 1/2/3 chunks can inherit fields from the most recent chunk on the
    // same chunk stream ID, and so a message can be assembled from chunks
    // spread across multiple on_bytes_received() calls.
    struct ChunkStreamState {
        bool has_header = false;
        std::uint32_t timestamp = 0;       // absolute timestamp baseline for delta application
        std::uint32_t timestamp_delta = 0; // last delta applied (fmt 1/2/3)
        std::uint32_t message_length = 0;
        std::uint8_t message_type_id = 0;
        std::uint32_t message_stream_id = 0;
        bool extended_timestamp = false; // whether the current header chain uses the extended field

        // In-progress message assembly (only meaningful while
        // bytes_remaining > 0).
        std::vector<std::byte> partial_payload;
        std::uint32_t bytes_remaining = 0;
        std::uint32_t message_timestamp = 0;
    };

    enum class DecodeResult : std::uint8_t { Consumed, InsufficientData, Failed };

    void fail(core::ErrorCode code, std::string_view message);
    DecodeResult decode_one();
    void handle_complete_message(std::uint32_t csid, ChunkStreamState& state);
    bool handle_protocol_control(const RtmpMessage& message);

    MessageHandler message_handler_;
    ErrorHandler error_handler_;

    std::vector<std::byte> buffer_;
    std::unordered_map<std::uint32_t, ChunkStreamState> streams_;

    std::uint32_t input_chunk_size_ = kDefaultChunkSize;
    std::uint32_t max_message_size_;
    std::uint64_t bytes_received_ = 0;
    std::uint32_t window_ack_size_ = 0;
    std::uint64_t bytes_at_last_ack_ = 0;
    bool failed_ = false;
};

} // namespace rtmp_server::protocol::chunk
