# Phase 3 Implementation Checklist — RTMP Chunk Engine

- [x] Chunk decoder — `chunk::ChunkDecoder` (`chunk_decoder.hpp`/`.cpp`),
  `on_bytes_received()` drives a byte-accumulating state machine
  (`decode_one()`), pure protocol logic, no sockets/io_uring
- [x] Chunk encoder — `chunk::ChunkEncoder` (`chunk_encoder.hpp`/`.cpp`),
  `encode_message()` picks the smallest valid fmt (0-3) per chunk stream ID
  and splits payload into `chunk_size()`-sized pieces
- [x] Basic Header 1/2/3-byte forms — `ChunkDecoder::decode_one` /
  `ChunkEncoder::write_basic_header`
- [x] Message Header types 0/1/2/3 (11/7/3/0 bytes) with per-chunk-stream-ID
  inheritance — `ChunkDecoder::ChunkStreamState` / `ChunkEncoder::ChunkStreamState`
- [x] Extended timestamp (>= 0x00FFFFFF), including the fmt3-repeats-the-field
  rule — `needs_extended`/`extended_timestamp` in both decoder and encoder
- [x] Configurable input chunk size (`input_chunk_size()`, updated by Set
  Chunk Size) and output chunk size (`ChunkEncoder::set_chunk_size`)
- [x] Protocol-control messages — Set Chunk Size and Abort Message handled
  internally by the decoder; Acknowledgement, Window Acknowledgement Size,
  Set Peer Bandwidth delivered to the caller (Window Ack Size additionally
  updates decoder state); `ChunkEncoder` has one helper per message
- [x] Acknowledgement-byte tracking — `ChunkDecoder::bytes_received()`,
  `window_acknowledgement_size()`, `acknowledgement_due()`, `mark_acknowledged()`
- [x] Maximum assembled message size enforcement — rejected with
  `ErrorCode::MessageTooLarge` before any oversized payload is buffered
  (`ChunkDecoderTest.OversizedMessageIsRejectedCleanly`)
- [x] Tests — 29 new GoogleTest cases (19 decoder + 10 encoder, including
  full encoder->decoder round-trip tests), see below

## Files created

- `include/rtmp_server/protocol/chunk/chunk_types.hpp`
- `include/rtmp_server/protocol/chunk/chunk_decoder.hpp`
- `include/rtmp_server/protocol/chunk/chunk_encoder.hpp`
- `src/protocol/chunk/chunk_decoder.cpp`
- `src/protocol/chunk/chunk_encoder.cpp`
- `tests/protocol/chunk/chunk_decoder_test.cpp`
- `tests/protocol/chunk/chunk_encoder_test.cpp`
- `docs/phase3-checklist.md` (this file)

## Files changed

- `src/protocol/CMakeLists.txt` — added `chunk/chunk_decoder.cpp` and
  `chunk/chunk_encoder.cpp` to `rtmp_server_protocol`
- `tests/protocol/CMakeLists.txt` — added `chunk/chunk_decoder_test.cpp` and
  `chunk/chunk_encoder_test.cpp` to `rtmp_server_protocol_tests`
- `docs/chunk-parser.md` — replaced the Phase 0 stub with full documentation
  of the wire format, header inheritance, extended timestamps, chunk sizing,
  protocol-control handling, and acknowledgement tracking

No `event_loop.cpp` / io_uring wiring was touched in this phase — Phase 2
already wired handshake completion to hand off into chunk-stream processing
being the next step, and actually plumbing `ChunkDecoder`/`ChunkEncoder`
into `IoUringEventLoop` and the connection lifecycle is deferred to a later
phase (or a follow-up pass) since it cannot be built or verified on this
host, matching the Phase 2 checklist's own scoping decision to keep
protocol-layer work and transport-layer wiring separate changes.

## Build and test evidence

Host: macOS/Darwin (no `io_uring`/Linux available), so the `core-only`
CMake preset (`RTMP_SERVER_CORE_ONLY=ON`) was used, same as Phase 2.

```
$ cmake --preset core-only
$ cmake --build --preset core-only
[6/6] Linking CXX executable tests/protocol/rtmp_server_protocol_tests
$ ctest --preset core-only
100% tests passed, 0 tests failed out of 57
```

57 total: 16 pre-existing Phase 0/1 core tests + 12 Phase 2 protocol
(handshake) tests + 29 new Phase 3 chunk tests (`ChunkDecoderTest` x19,
`ChunkEncoderTest` x10).

Also configured and built the `asan` preset with
`RTMP_SERVER_CORE_ONLY=ON` (`-fsanitize=address,undefined`):

```
$ cmake --preset asan -DRTMP_SERVER_CORE_ONLY=ON
$ cmake --build --preset asan
[6/6] Linking CXX executable tests/protocol/rtmp_server_protocol_tests
$ ctest --preset asan
100% tests passed, 0 tests failed out of 57
```

Same 57/57 pass clean under ASan+UBSan — no leaks, no undefined behavior,
no heap corruption across any chunk decode/encode path exercised by the
tests, including the byte-at-a-time and oversized-message paths that stress
the buffer-management code most directly.

## Known limitations

- `ChunkDecoder`/`ChunkEncoder` are pure protocol-layer components; nothing
  in this phase wires them into `IoUringEventLoop` or a per-connection
  session object analogous to `HandshakeSession`'s transport wiring. That
  integration (deciding when a connection transitions from handshake to
  chunk-stream mode, driving acknowledgement sends off actual socket
  writes, backpressure when a send queue is full, etc.) is left for the
  phase that adds AMF0/command handling and stream registration, since it
  needs those pieces to be meaningful (a connection with only a chunk
  engine and no command dispatch cannot do anything RTMP-observable yet).
- fmt3 chunks that start a *new* message (as opposed to continuing an
  in-progress one) are decoded by reusing the previous delta on that chunk
  stream ID, per spec. If a peer sends a degenerate fmt3-starts-new-message
  chunk on a chunk stream ID whose previous message used fmt0 (i.e. no
  delta was ever established, `timestamp_delta` defaults to 0), the decoded
  timestamp is `previous_timestamp + 0` (no advance). This matches common
  server implementations but is not distinguishable on the wire from "the
  timestamp genuinely did not advance" — an inherent ambiguity in the RTMP
  chunk format itself, not a decoder defect.
- User Control Message (type 4) and Set Peer Bandwidth (type 6) payloads
  are decoded only as far as generic message framing goes; this phase does
  not parse their internal sub-fields (event type for User Control, limit
  type is exposed for Set Peer Bandwidth via `PeerBandwidthLimitType` only
  on the encode side). Acting on Acknowledgement/User Control/Set Peer
  Bandwidth content is session-level logic properly belonging to a later
  phase once there is a session to act on it.
- No fuzz target yet for `ChunkDecoder::on_bytes_received` (the "Fuzz
  tests" section of `docs/rtmp_promot.md` is a later, cross-cutting
  requirement, not scoped to Phase 3 specifically).

Next: Phase 4 — AMF0 and RTMP Commands.
