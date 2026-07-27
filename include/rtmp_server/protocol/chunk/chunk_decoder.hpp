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
// Per-connection reassembly bounds (Phase 8 security tasks 5 and 6,
// docs/v2_promot.md 3.5 "Chunk size" / "Per-connection receive buffer").
//
// Why these exist: the RTMP basic header can address 65600 distinct chunk
// stream IDs, and a peer may legally start a message on each of them and
// leave every one incomplete. Before Phase 8 the decoder created a
// ChunkStreamState per csid on demand and immediately called
// partial_payload.reserve(declared_message_length) — so a peer could
// declare the full max_message_size on thousands of chunk streams and make
// the server allocate gigabytes. Measured on the pre-fix code: 2.84 MB of
// input drove 269 MiB RSS / 792 MiB peak footprint in a single decoder
// (docs/security.md "Chunk-stream reassembly amplification").
struct ChunkDecoderLimits {
    // Concurrently tracked chunk stream IDs. Real publishers use a handful
    // (2 = protocol control, 3 = command, 4/5/6 = audio/video/data is the
    // conventional layout used by OBS, FFmpeg and librtmp); 64 leaves an
    // order of magnitude of headroom while capping the state table.
    std::uint32_t max_chunk_streams = 64;

    // Total bytes held across every chunk stream's in-progress payload on
    // this connection. Two full maximum-size messages' worth of headroom
    // lets one big message be reassembled while another chunk stream is
    // mid-message, which is the deepest legitimate interleaving; beyond
    // that the peer is hoarding, not streaming.
    std::uint64_t max_buffered_payload_bytes = 2ULL * 10 * 1024 * 1024;

    // Largest Set Chunk Size the peer may negotiate. The RTMP specification
    // caps the field at 0xFFFFFF, and a chunk can never usefully exceed the
    // largest message we accept, so the effective cap is
    // min(this, max_message_size). Bounding it keeps the raw input buffer
    // (which must hold one whole chunk before a slice can be committed)
    // proportional to the message limit rather than to a 2 GiB field.
    std::uint32_t max_chunk_size = 0xFFFFFF;

    // Upfront reservation cap for a new message's payload. The declared
    // length is validated against max_message_size, but reserving it in one
    // go still lets a 15-byte header commit max_message_size of memory. The
    // vector grows geometrically from here instead, so a peer only ever
    // gets memory proportional to bytes it actually sent.
    std::size_t initial_payload_reserve = 64u * 1024u;
};

class ChunkDecoder {
public:
    using MessageHandler = std::function<void(RtmpMessage)>;
    using ErrorHandler = std::function<void(core::Error)>;

    explicit ChunkDecoder(std::uint32_t max_message_size, ChunkDecoderLimits limits = {});

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

    [[nodiscard]] const ChunkDecoderLimits& limits() const noexcept { return limits_; }

    // Bytes currently held across every chunk stream's in-progress payload
    // (the quantity bounded by limits().max_buffered_payload_bytes).
    // Exposed for tests and for connection-level memory accounting.
    [[nodiscard]] std::uint64_t buffered_payload_bytes() const noexcept { return buffered_payload_bytes_; }

    // Number of chunk stream IDs currently tracked.
    [[nodiscard]] std::size_t chunk_stream_count() const noexcept { return streams_.size(); }

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

    ChunkDecoderLimits limits_;
    std::uint32_t input_chunk_size_ = kDefaultChunkSize;
    std::uint32_t max_message_size_;
    std::uint64_t buffered_payload_bytes_ = 0;
    std::uint64_t bytes_received_ = 0;
    std::uint32_t window_ack_size_ = 0;
    std::uint64_t bytes_at_last_ack_ = 0;
    bool failed_ = false;
};

} // namespace rtmp_server::protocol::chunk
