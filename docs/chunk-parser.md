# Chunk Parser

> Status: implemented in Phase 3. See `docs/phase3-checklist.md` for the
> phase tracking record.

## Scope

The RTMP Chunk Protocol: the framing layer that carries every message
(protocol-control, AMF0 commands, audio, video, ...) once the handshake
(`docs/rtmp-handshake.md`) has completed. Implemented by two independent,
stateful, allocation-conscious classes with no socket/io_uring dependency —
consistent with `protocol::handshake::HandshakeSession`
(`docs/architecture.md` "Architectural Separation"):

* `rtmp_server::protocol::chunk::ChunkDecoder`
  (`include/rtmp_server/protocol/chunk/chunk_decoder.hpp`,
  `src/protocol/chunk/chunk_decoder.cpp`) — turns a byte stream (arbitrarily
  fragmented) into a sequence of complete `RtmpMessage`s.
* `rtmp_server::protocol::chunk::ChunkEncoder`
  (`include/rtmp_server/protocol/chunk/chunk_encoder.hpp`,
  `src/protocol/chunk/chunk_encoder.cpp`) — turns `RtmpMessage`s into
  chunk-framed bytes ready to hand to a transport's `async_write`.

Shared wire constants, the `RtmpMessage` struct, and the `MessageTypeId` /
`PeerBandwidthLimitType` enums live in
`include/rtmp_server/protocol/chunk/chunk_types.hpp`.

## Wire format

### Basic Header (1, 2, or 3 bytes)

```text
byte0 bits 7-6: fmt (0-3)
byte0 bits 5-0: chunk stream ID (csid) field

csid field == 0  -> 2-byte form: csid = 64 + byte1
csid field == 1  -> 3-byte form: csid = 64 + byte1 + byte2*256
csid field 2..63 -> 1-byte form: csid = csid field (values 0 and 1 are
                    reserved as the escape markers above, so csid 2 is the
                    lowest value ever encoded in the 1-byte form)
```

`ChunkEncoder` always emits the smallest form that fits a given csid;
`ChunkDecoder` accepts all three on input.

### Message Header (11 / 7 / 3 / 0 bytes, selected by fmt)

```text
fmt 0 (11 bytes): timestamp(3, BE) message_length(3, BE) message_type_id(1)
                  message_stream_id(4, LE)
fmt 1 (7 bytes):  timestamp_delta(3, BE) message_length(3, BE) message_type_id(1)
fmt 2 (3 bytes):  timestamp_delta(3, BE)
fmt 3 (0 bytes):  (nothing — every field inherited)
```

`message_stream_id` is the one field the RTMP spec encodes little-endian;
every other multi-byte field on the wire (including AMF0 numbers, unrelated
to this layer) is big-endian, per `core/byte_order.hpp`.

### Header inheritance

Fields not present in a given fmt are inherited from the **most recent
header seen on the same chunk stream ID** (independent per csid — this is
exactly what makes interleaving multiple chunk streams over one TCP
connection work):

* fmt 1 inherits `message_stream_id`.
* fmt 2 inherits `message_stream_id`, `message_length`, `message_type_id`.
* fmt 3 inherits everything, including the timestamp delta itself. A fmt3
  chunk means one of two things, disambiguated by whether a message is
  currently mid-assembly on that csid: (a) a **continuation** chunk of an
  in-progress message (no timestamp change — the chunk is just more bytes of
  the same message), or (b) the **start of a new message** that reuses the
  previous message's delta verbatim (this is the common "steady frame rate"
  case: every video/audio message uses the same delta as the last).

The first chunk ever seen on a chunk stream ID must be fmt 0 — there is
nothing to inherit from. `ChunkDecoder` rejects any other fmt in that
position with `ErrorCode::MalformedChunk`.

### Extended timestamp

If a timestamp (fmt0) or delta (fmt1/2) does not fit in the 3-byte field
(i.e. is `>= 0x00FFFFFF`), the field is written as the literal marker value
`0x00FFFFFF` and a 4-byte big-endian absolute value follows immediately
after the message header. Per spec, **once a chunk stream's timestamp
requires the extended field, every subsequent chunk on that stream —
including fmt3 chunks — repeats the 4-byte field** for as long as that
holds. `ChunkStreamState::extended_timestamp` (decoder) /
`extended_timestamp`+`extended_value` (encoder) track this per csid so fmt3
chunks know whether to expect/emit it and what value to (re-)use.

### Message reassembly and chunking

A message's payload (`message_length` bytes) is split across one or more
chunks: the first carries the full basic+message header (fmt 0/1/2), and
every subsequent chunk for the same message carries only a fmt3 basic
header (+ repeated extended timestamp field if active) followed by up to
`input_chunk_size()` more payload bytes. `ChunkDecoder` accumulates these
into a per-csid `partial_payload` buffer and only invokes the message
handler once the full `message_length` bytes have arrived — which may span
any number of `on_bytes_received()` calls, down to one byte at a time.

`ChunkDecoder::decode_one()` never mutates its per-csid state or the
pending-input buffer until it has confirmed the *entire* next chunk (basic
header + message header + optional extended timestamp + payload slice) is
present in the buffer. If not, it leaves everything untouched and returns
"insufficient data", so a caller feeding bytes one at a time gets
byte-exact, idempotent behavior with no partial/corrupt state.

## Chunk size

* `input_chunk_size()` (decoder) governs how many payload bytes it expects
  per chunk from the peer; it starts at `kDefaultChunkSize` (128, per spec)
  and is updated whenever a Set Chunk Size protocol-control message is
  decoded.
* `chunk_size()` (encoder, settable via `set_chunk_size()`) governs how the
  encoder slices outgoing message payloads. Changing it does **not**
  automatically notify the peer — callers must also call
  `encode_set_chunk_size()` and send that message, exactly as the peer must
  do for the decoder side to track it correctly (see
  `ChunkEncoderTest.RoundTripThroughDecoderPreservesArbitraryMessageSequence`
  for a worked example).

## Protocol-control messages

Always sent on chunk stream ID 2 (`kProtocolControlChunkStreamId`), message
stream ID 0 (`kProtocolControlMessageStreamId`):

| Message                      | Type ID | Payload                              | Decoder behavior |
|-------------------------------|---------|---------------------------------------|-------------------|
| Set Chunk Size                | 1       | u32 (top bit reserved 0)              | Applied internally to `input_chunk_size_`; not delivered to the message handler. |
| Abort Message                 | 2       | u32 target chunk stream ID            | Discards that csid's in-progress `partial_payload`/`bytes_remaining`; not delivered. |
| Acknowledgement                | 3       | u32 sequence number                   | Delivered to the message handler (session-level backpressure logic, out of scope for the chunk layer itself). |
| User Control Message           | 4       | event type + event data               | Delivered to the message handler unmodified. |
| Window Acknowledgement Size    | 5       | u32 window size                       | Updates `window_acknowledgement_size()` **and** is delivered to the message handler. |
| Set Peer Bandwidth             | 6       | u32 window size + u8 limit type        | Delivered to the message handler unmodified. |

`ChunkEncoder` provides one helper per message
(`encode_set_chunk_size`, `encode_abort_message`, `encode_acknowledgement`,
`encode_window_acknowledgement_size`, `encode_set_peer_bandwidth`) that
builds the `RtmpMessage` and encodes it in one call.

## Acknowledgement-byte tracking

`ChunkDecoder::bytes_received()` counts every raw byte ever handed to
`on_bytes_received()`. `window_acknowledgement_size()` is set automatically
from an incoming Window Acknowledgement Size message.
`acknowledgement_due()` returns true once `bytes_received() -
<bytes at last mark_acknowledged() call>` reaches the window. The
session-level caller is expected to: check `acknowledgement_due()`
periodically (e.g. after each `on_bytes_received()`), and if true, encode
and send an Acknowledgement message via
`ChunkEncoder::encode_acknowledgement(decoder.bytes_received(), ...)` and
then call `decoder.mark_acknowledged()`.

## Maximum message size

`ChunkDecoder` is constructed with a `max_message_size` (bytes). The
`message_length` field of any fmt0/fmt1 header that would start a message
exceeding this is rejected immediately with `ErrorCode::MessageTooLarge`
— before any payload bytes for it are even required to be present in the
buffer, so a malicious or buggy peer cannot force unbounded buffering by
lying about a huge length up front. Once `ChunkDecoder` has failed
(`failed()` returns true — this is also the outcome of any
`ErrorCode::MalformedChunk`), it discards its buffered state and ignores
all further input; the caller is expected to close the connection.

## Known limitations

See `docs/phase3-checklist.md` "Known limitations".
