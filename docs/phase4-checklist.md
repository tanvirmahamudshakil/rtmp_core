# Phase 4 Implementation Checklist — AMF0 and RTMP Commands

- [x] AMF0 encoder/decoder — `amf0::Amf0Value` (`amf0_value.hpp`),
  `amf0::encode()` (`amf0_encoder.hpp`/`.cpp`), `amf0::decode()`/
  `decode_all()` (`amf0_decoder.hpp`/`.cpp`); Number, Boolean, String
  (short + Long String wire forms), Object, Null, Undefined, ECMA Array,
  Strict Array, and Date all supported
- [x] connect — `CommandSession::handle_connect`, replies `_result` with
  `NetConnection.Connect.Success`
- [x] releaseStream — `CommandSession::handle_release_stream`, replies
  `_result`
- [x] FCPublish — `CommandSession::handle_fc_publish`, replies `_result`
- [x] createStream — `CommandSession::handle_create_stream`, assigns and
  returns a new numeric stream ID via `_result`
- [x] publish — `CommandSession::handle_publish`, validates the stream key
  via an injected `StreamKeyValidator`, registers the stream in
  `StreamRegistry`, transitions to `NetStreamState::Publishing`
- [x] play — `CommandSession::handle_play`, transitions to
  `NetStreamState::Playing`, replies `onStatus` with `NetStream.Play.Start`
- [x] close/delete stream — `CommandSession::handle_delete_stream`
  (`deleteStream` command) and `CommandSession::on_connection_closed()`
  (abrupt disconnect), both unregister from `StreamRegistry`
- [x] result/status responses — `send_result`/`send_error`/`send_status`
  producing the standard `_result`/`_error`/`onStatus` AMF0 command shapes
- [x] Tests — 35 new GoogleTest cases (23 AMF0 codec + 12 command session),
  see below

## Files created

- `include/rtmp_server/protocol/amf0/amf0_value.hpp`
- `include/rtmp_server/protocol/amf0/amf0_encoder.hpp`
- `include/rtmp_server/protocol/amf0/amf0_decoder.hpp`
- `src/protocol/amf0/amf0_encoder.cpp`
- `src/protocol/amf0/amf0_decoder.cpp`
- `include/rtmp_server/protocol/commands/command_session.hpp`
- `include/rtmp_server/protocol/commands/stream_registry.hpp`
- `src/protocol/commands/command_session.cpp`
- `tests/protocol/amf0/amf0_codec_test.cpp`
- `tests/protocol/commands/command_session_test.cpp`
- `docs/rtmp-commands.md`
- `docs/phase4-checklist.md` (this file)

## Files changed

- `src/protocol/CMakeLists.txt` — added `amf0/amf0_encoder.cpp`,
  `amf0/amf0_decoder.cpp`, `commands/command_session.cpp` to
  `rtmp_server_protocol`
- `tests/protocol/CMakeLists.txt` — added `amf0/amf0_codec_test.cpp` and
  `commands/command_session_test.cpp` to `rtmp_server_protocol_tests`
- `docs/amf0.md` — replaced the Phase 0 stub with full documentation of the
  value model, wire format, and the shared_ptr-indirection design decision
  for recursive Object/Array storage

`StreamRegistry` was placed under `include/rtmp_server/protocol/commands/`
rather than `include/rtmp_server/server/registry/` (which exists but is
still an empty `.gitkeep` placeholder): the existing
`server/connection/connection_registry.hpp` already depends on
`network::TcpConnection`, which doesn't build under
`RTMP_SERVER_CORE_ONLY`, and `StreamRegistry` needs to be usable and
testable on this host the same way `CommandSession` is. It's
transport-independent (keyed by a plain `connection_id`), so nothing is
lost by keeping it in the protocol layer for now; see
`docs/rtmp-commands.md` "Stream key validation and the registry" for the
full reasoning. Later phases are free to move or wrap it under `server/`
once real connection wiring exists.

No `event_loop.cpp`/io_uring wiring was touched in this phase, same
deferral as Phase 2 and Phase 3.

## Acceptance criteria evidence

- **OBS receives connection success** —
  `CommandSessionTest.ConnectProducesResultWithConnectSuccessStatus`:
  sends a `connect` command, asserts the reply is `_result` whose
  information object has `code == "NetConnection.Connect.Success"` and
  `level == "status"`.
- **OBS creates a stream** —
  `CommandSessionTest.CreateStreamProducesResultWithNumericStreamId`:
  sends `createStream`, asserts the reply is `_result` with a positive
  numeric stream ID, and that it matches `last_created_stream_id()`.
- **valid key can publish** —
  `CommandSessionTest.ValidKeyPublishProducesPublishStartStatus`: publish
  with an authorized key produces `onStatus`/`NetStream.Publish.Start` and
  `stream_state() == Publishing`.
- **invalid key is rejected** —
  `CommandSessionTest.InvalidKeyPublishIsRejectedAndDoesNotTransitionToPublishing`:
  publish with an unauthorized key produces `onStatus`/
  `NetStream.Publish.BadName` (level `"error"`), `stream_state() !=
  Publishing`, and the key does not appear in the registry. A second,
  distinct case (`SecondPublisherForSameKeyIsRejected`) covers a *valid*
  key already held by another connection being rejected the same way,
  without disturbing the original publisher's registration.
- **stream appears in registry** —
  `CommandSessionTest.PublishedStreamAppearsInRegistry`: after a
  successful publish, `StreamRegistry::is_published()`/`find()` return the
  expected `connection_id`, `stream_id`, and `app` — this is the data
  structure a later Playback phase will read from.

## Build and test evidence

Host: macOS/Darwin (no `io_uring`/Linux available), so the `core-only`
CMake preset (`RTMP_SERVER_CORE_ONLY=ON`) was used, same as Phase 2/3.

```
$ cmake --preset core-only
$ cmake --build --preset core-only
[7/7] Linking CXX executable tests/protocol/rtmp_server_protocol_tests
$ ctest --preset core-only
100% tests passed, 0 tests failed out of 92
```

92 total: 57 pre-existing (16 Phase 0/1 core + 12 Phase 2 handshake + 29
Phase 3 chunk) + 35 new Phase 4 tests (`Amf0CodecTest` x23,
`CommandSessionTest` x12).

Also configured and built the `asan` preset with
`RTMP_SERVER_CORE_ONLY=ON` (`-fsanitize=address,undefined`):

```
$ cmake --preset asan -DRTMP_SERVER_CORE_ONLY=ON
$ cmake --build --preset asan
[7/7] Linking CXX executable tests/protocol/rtmp_server_protocol_tests
$ ctest --preset asan
100% tests passed, 0 tests failed out of 92
```

Same 92/92 pass clean under ASan+UBSan — no leaks, no undefined behavior,
no heap corruption, including the `shared_ptr`-based recursive AMF0 value
storage and the malformed-input rejection paths.

## Known limitations

- Not wired into `event_loop.cpp`/`IoUringEventLoop` — see "Files changed"
  above and `docs/rtmp-commands.md` "Known limitations" for the full
  reasoning; matches Phase 2/3's own scoping decision.
- AMF3 (message type 17, "AMF3 Command") is not implemented; this server
  always advertises AMF0 (`objectEncoding: 0`) and does not understand a
  client that only speaks AMF3. Not required by the phase spec.
- `play`'s optional `start`/`duration`/`reset` arguments are accepted but
  not acted on — actually serving a stream is Phase 6 (Playback); this
  phase only implements the command handshake (`onStatus` with
  `NetStream.Play.Start`).
- `deleteStream` sends no reply (matches common real-server behavior —
  there's no standard `_result`/`onStatus` shape clients expect for it).
- `StreamRegistry` is intentionally minimal (stream key -> publishing
  connection id / stream id / app / start time only) — no GOP cache
  handle, no subscriber list, no bitrate counters; those belong to Media
  Ingest and Playback (Phases 5-6), which are expected to extend it rather
  than replace it.
- No fuzz target yet for `amf0::decode`/`decode_all` — same deferral as
  Phase 3's `ChunkDecoder` (a later, cross-cutting phase per
  `docs/rtmp_promot.md` "Fuzz tests").
- No live OBS/ffmpeg instance was available to test against; the
  acceptance criteria are verified by GoogleTest cases that construct
  `chunk::RtmpMessage`s carrying real AMF0-encoded command payloads byte
  for byte as OBS/FMLE would send them, and decode/inspect the AMF0
  responses `CommandSession` produces, mirroring how Phase 2 substituted a
  scripted real-socket handshake test for a live OBS instance.

Next: Phase 5 — Media Ingest.
