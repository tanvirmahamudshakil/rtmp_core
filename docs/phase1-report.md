# PHASE 1 COMPLETION REPORT

## 1. What was inspected

* `docs/v2_promot.md` (master prompt) in full, plus the Phase 0 artifacts:
  `docs/phase0-v2-report.md`, `docs/current-architecture.md`,
  `docs/production-gap-analysis.md`, `docs/target-architecture.md`.
* `include/rtmp_server/protocol/chunk/chunk_decoder.hpp` and
  `src/protocol/chunk/chunk_decoder.cpp` (full decode loop, protocol-control
  handling, max-message-size enforcement, malformed-chunk rejection).
* `include/rtmp_server/protocol/chunk/chunk_encoder.hpp`.
* `include/rtmp_server/protocol/commands/command_session.hpp` and
  `src/protocol/commands/command_session.cpp` (connect/createStream/
  publish/play/deleteStream/releaseStream/FCPublish dispatch, media routing,
  playback relay, connection-close cleanup).
* `include/rtmp_server/protocol/handshake/handshake_session.hpp` and
  `.cpp` (state machine, C0/C1/C2 buffer handling).
* `include/rtmp_server/protocol/commands/stream_registry.hpp`,
  `live_fanout.hpp`, `media_ingest.hpp`, `recorder_sink.hpp`.
* `include/rtmp_server/io/io_uring/event_loop.hpp` and
  `src/io/io_uring/event_loop.cpp` (full accept/handshake/receive/send/
  timeout/shutdown wiring).
* `include/rtmp_server/network/tcp_connection.hpp` and `.cpp` (write queue,
  receive handler, close/peer-closed paths).
* `apps/rtmp_server/CMakeLists.txt`, `apps/rtmp_server/main.cpp`,
  `src/protocol/CMakeLists.txt`, top-level `CMakeLists.txt`.
* `tests/protocol/commands/command_session_test.cpp` and
  `tests/protocol/handshake/handshake_socket_integration_test.cpp` for the
  existing testing patterns/conventions this phase's tests follow.

## 2. Problems confirmed

All Phase 0 findings relevant to Phase 1 were reconfirmed by reading the
code before making any change, matching `docs/production-gap-analysis.md`:

* `apps/rtmp_server` linked only `rtmp_server_core` + `rtmp_server_io_uring`,
  not `rtmp_server_protocol` — confirmed in `apps/rtmp_server/CMakeLists.txt`.
* `IoUringEventLoop::start_handshake`'s `complete_handler_`
  (`event_loop.cpp:364-375`, pre-change) never replaced the receive handler
  installed at accept time — confirmed.
* `HandshakeSession::try_consume_c2` left any bytes after C2 in `buffer_`
  with no accessor to retrieve them, and no caller ever read them — confirmed
  (see item 4, a new finding, below).
* No file under `src/io`/`src/network`/`apps/rtmp_server` referenced
  `ChunkDecoder`/`ChunkEncoder`/`CommandSession` — confirmed by grep before
  this phase's changes.

**New finding during this phase (not in the Phase 0 report):** the RTMP
chunk protocol requires the sender to announce any non-default chunk size
via a `Set Chunk Size` protocol-control message before using it — a
decoder that hasn't been told otherwise keeps assuming
`chunk::kDefaultChunkSize` (128 bytes) forever, regardless of what chunk
size the encoder on the other side actually used. `ServerConfig::
output_chunk_size` defaults to 4096. Neither `ChunkEncoder` nor
`CommandSession` sent this message anywhere in the codebase prior to this
phase — meaning that even once the pipeline was wired up, a real RTMP
client's decoder would have silently stalled forever trying to reassemble
the very first reply larger than one default-size chunk (i.e. essentially
every real reply). This was caught by
`RtmpFullSessionSocketIntegration.HandshakePlusConnectInSameWriteReachesTheSession`
(a real-socket test, not the in-process ones) during development — see
Architecture decisions below for the fix.

## 3. Problems not confirmed

None of the Phase 0 findings assigned to Phase 1's scope were found to be
incorrect. `FCUnpublish` (task 8, "where supported") remains intentionally
unhandled: `CommandSession::dispatch` silently ignores unrecognized command
names, and `deleteStream` (which OBS and other real publishers always send
on stop, alongside or instead of `FCUnpublish`) already performs full
unpublish cleanup via `StreamRegistry::unregister_publisher` and
`LiveFanout::publisher_stopped`. Adding an explicit `FCUnpublish` handler
would be a same-shape addition to `handle_release_stream`/`handle_fc_publish`
but was judged out of the minimal-diff scope for this phase since it is not
required for OBS/ffmpeg interoperability and is not exercised by the
Required Tests list; flagged as a candidate for a later phase's polish pass.

## 4. Architecture decisions

* **`RtmpConnectionSession` is transport-agnostic**, mirroring
  `HandshakeSession`/`ChunkDecoder`/`CommandSession`: it takes byte spans in
  (`on_bytes_received`) and produces already chunk-encoded byte vectors out
  (`OutgoingBytesHandler`), with no socket or `io_uring` dependency. This is
  what makes the Required Tests runnable on macOS, where the `io_uring`
  transport target itself cannot be built
  (`CMAKE_SYSTEM_NAME STREQUAL "Linux"` guard, unchanged from Phase 0). It
  lives in `rtmp_server_protocol`, next to `CommandSession`.
* **Ownership**: `RtmpConnectionSession` owns one `ChunkDecoder`, one
  `ChunkEncoder`, and one `CommandSession` by value (not shared/pointer) —
  each connection gets its own instance of each. It holds non-owning
  pointers to the process-wide `StreamRegistry`/`LiveFanout`
  (`Dependencies` struct), matching how `CommandSession` already took a
  `StreamRegistry&` and optional non-owning `LiveFanout*`/`MediaIngest*`/
  `RecorderSink*`. `IoUringEventLoop` now owns exactly one
  `StreamRegistry` and one `LiveFanout` for the whole process (Phase 3 will
  decide whether/how to shard this across workers — out of scope here).
* **`IoUringEventLoop` owns each connection's `RtmpConnectionSession`** in a
  new `rtmp_sessions_` map keyed by `connection_id`, exactly parallel to the
  existing `handshake_sessions_` map — constructed in the new
  `start_rtmp_session()` (called from the handshake's `complete_handler_`)
  and destroyed in the new `cleanup_rtmp_session_state()` (called from
  `close_connection()`, alongside the existing `cleanup_handshake_state()`).
* **Handshake trailing bytes**: added
  `HandshakeSession::take_trailing_bytes()` — `try_consume_c2()` already
  left any bytes after C2 in `buffer_` untouched (it only erases what it
  consumes), so the fix is exposing an accessor rather than changing the
  state machine. `IoUringEventLoop::start_handshake`'s `complete_handler_`
  now calls this *before* `cleanup_handshake_state()` drops the session, and
  passes the result into `start_rtmp_session()`, which feeds it through the
  new session's `on_bytes_received()` after every handler is wired — so a
  client that pipelines `connect` onto the same write as C2 (a real,
  observed OBS behavior) is handled correctly instead of losing those bytes.
* **`Set Chunk Size` on session start**: `RtmpConnectionSession::start()` is
  a new, explicit method (must be called once, after `set_outgoing_handler`,
  before any bytes are fed in) that sends a `Set Chunk Size` control message
  when constructed with a non-default output chunk size. This is
  deliberately not done implicitly in the constructor, because the outgoing
  handler is not necessarily wired yet at construction time (both the real
  `IoUringEventLoop::start_rtmp_session` and the test harnesses build the
  session, then wire handlers, then start it) — an implicit constructor-time
  send would silently be lost. `IoUringEventLoop::start_rtmp_session` calls
  `start()` after every handler is set and before the receive handler swap,
  so nothing the peer sends can race ahead of it.
* **Control messages** (`Set Peer Bandwidth`, `Window Acknowledgement Size`
  handling, `Acknowledgement`, `User Control` Ping Request/Response) are
  handled in `RtmpConnectionSession`, not `CommandSession`, because
  `ChunkDecoder` already explicitly documents these as "session-level"
  concerns it deliberately does not own
  (`chunk_decoder.hpp`: "Acknowledgement and Set Peer Bandwidth ... simply
  delivered to the message handler ... a session-level (not chunk-level)
  concern"), and `CommandSession`'s own doc comment scopes it to AMF
  command/media routing only. Putting protocol-control handling in a
  dedicated layer keeps `CommandSession` unchanged and matches the target
  pipeline diagram in `docs/v2_promot.md` Phase 1 ("Control message
  handler" as a sibling of `CommandSession`, not a part of it).
* **Not implemented in this phase**: an unsolicited server-initiated
  `Ping Request` (keepalive) — liveness is already covered by
  `IoUringEventLoop::arm_idle_timeout`, and the Required Tests only ask for
  ping/pong (i.e. responding to a client's ping), which is implemented.
  Documented as a deliberate scope decision, not an oversight.
* **`TcpConnection::pending_write_bytes()`** was added (sums queued
  `SharedBuffer::size()`) so `CommandSession::set_pending_bytes_provider`
  (already existing playback-backpressure hook) has a real value to read
  from the actual connection instead of nothing. The write queue itself
  becoming byte/packet-bounded is explicitly a Phase 2/3 concern (Phase 1
  task list does not include it); this only wires the existing accounting
  through.

## 5. Files added

* `include/rtmp_server/protocol/session/rtmp_connection_session.hpp`
* `src/protocol/session/rtmp_connection_session.cpp`
* `tests/integration/rtmp_connection_session_test.cpp`
* `tests/integration/rtmp_full_session_socket_test.cpp`
* `tests/integration/CMakeLists.txt`
* `docs/phase1-report.md` (this file)

## 6. Files modified

* `include/rtmp_server/protocol/handshake/handshake_session.hpp` —
  added `take_trailing_bytes()`.
* `include/rtmp_server/io/io_uring/event_loop.hpp` — added
  `stream_registry_`/`live_fanout_`/`rtmp_sessions_` members and
  `start_rtmp_session()`/`cleanup_rtmp_session_state()` declarations.
* `src/io/io_uring/event_loop.cpp` — `start_handshake`'s `complete_handler_`
  now takes trailing bytes and calls `start_rtmp_session`; added
  `start_rtmp_session()`/`cleanup_rtmp_session_state()` definitions;
  `close_connection()` now also calls `cleanup_rtmp_session_state()`.
* `include/rtmp_server/network/tcp_connection.hpp` — added
  `pending_write_bytes()`.
* `src/protocol/CMakeLists.txt` — added `session/rtmp_connection_session.cpp`
  to `rtmp_server_protocol`.
* `apps/rtmp_server/CMakeLists.txt` — links `rtmp_server_protocol` (was
  missing entirely, per Phase 0 finding #12/#2).
* `CMakeLists.txt` (top level) — added `add_subdirectory(tests/integration)`.

`rtmp_server_management` is still **not** linked into `apps/rtmp_server` —
authentication/token validation is explicitly Phase 5 scope
(`docs/v2_promot.md` Phase 1 task list has no publish-key-authentication
task; `StreamKeyValidator` remains an injectable hook, defaulted to
"always allow" by `RtmpConnectionSession` when none is supplied).

## 7. Public interfaces changed

* New public type: `rtmp_server::protocol::session::RtmpConnectionSession`
  (constructor, `set_outgoing_handler`, `set_close_handler`, `start`,
  `set_pending_bytes_provider`, `set_max_queued_playback_bytes`,
  `on_bytes_received`, `on_connection_closed`, `failed()`,
  `command_session()`).
* `HandshakeSession::take_trailing_bytes()` — new public method, additive,
  no change to existing signatures.
* `TcpConnection::pending_write_bytes()` — new public method, additive.
* `IoUringEventLoop`'s public surface (`run`, `stop`, `connections()`,
  `submit_receive`/`submit_send`/`request_close`) is unchanged; the new
  members/methods are private implementation detail.

## 8. Tests added

`tests/integration/rtmp_connection_session_test.cpp` (in-process,
transport-agnostic, 9 tests):

* `ConnectCommandProducesConnectSuccessResult`
* `FragmentedRtmpChunksAreReassembledOneByteAtATime`
* `MultipleChunksInOneReceiveBufferAreAllProcessed`
* `CreateStreamPublishAndPlayFullLifecycle`
* `MalformedAmfPayloadIsDroppedWithoutCrashingOrClosing`
* `MessageExceedingConfiguredMaximumClosesConnection`
* `PingRequestProducesPingResponseWithSameTimestamp`
* `ConnectionCleanupUnregistersPublisherAndSubscriber`
* `HandshakeTrailingBytesArePreservedAndReachTheSession`

`tests/integration/rtmp_full_session_socket_test.cpp` (real loopback TCP
socket, plain blocking POSIX I/O — no `io_uring` — 2 tests):

* `HandshakePlusConnectInSameWriteReachesTheSession`
* `AbruptDisconnectDuringPublishCleansUpRegistry`

Coverage against the Phase 1 "Required tests" list:

| Required test | Covered by |
|---|---|
| Handshake only | Pre-existing `handshake_session_test.cpp`/`handshake_socket_integration_test.cpp` (unchanged, still passing) |
| Handshake plus `connect` in same receive buffer | `HandshakeTrailingBytesArePreservedAndReachTheSession`, `HandshakePlusConnectInSameWriteReachesTheSession` (real socket) |
| Fragmented handshake | Pre-existing `handshake_socket_integration_test.cpp` (unchanged) |
| Fragmented RTMP chunks | `FragmentedRtmpChunksAreReassembledOneByteAtATime` |
| Multiple chunks in one receive buffer | `MultipleChunksInOneReceiveBufferAreAllProcessed` |
| `connect` command | `ConnectCommandProducesConnectSuccessResult` |
| `createStream` | `MultipleChunksInOneReceiveBufferAreAllProcessed`, `CreateStreamPublishAndPlayFullLifecycle` |
| Valid publish | `CreateStreamPublishAndPlayFullLifecycle` |
| Valid play | `CreateStreamPublishAndPlayFullLifecycle` |
| Malformed AMF | `MalformedAmfPayloadIsDroppedWithoutCrashingOrClosing` |
| Message exceeding configured maximum | `MessageExceedingConfiguredMaximumClosesConnection` |
| Abrupt disconnect during handshake | Pre-existing `handshake_socket_integration_test.cpp` `RealLoopbackSocketRejectsInvalidVersion` (fail path); no new test added specifically for this exact wording since `HandshakeSession`'s disconnect-mid-handshake behavior did not change in this phase |
| Abrupt disconnect during publish | `AbruptDisconnectDuringPublishCleansUpRegistry` (real socket) |
| Abrupt disconnect during play | **Not covered by a dedicated test** — see Remaining risks |
| Ping/pong | `PingRequestProducesPingResponseWithSameTimestamp` |
| Connection cleanup | `ConnectionCleanupUnregistersPublisherAndSubscriber`, `AbruptDisconnectDuringPublishCleansUpRegistry` |

## 9. Commands executed

```
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DRTMP_SERVER_ENABLE_ASAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

## 10. Actual build result

Both configurations built successfully with **zero warnings and zero
errors** in project code (the only warnings present are the pre-existing,
Phase-0-documented `ld: warning: ignoring duplicate libraries:
'../src/protocol/librtmp_server_protocol.a'` linker warnings from the fuzz
targets, unrelated to this phase). `apps/rtmp_server` and
`rtmp_server_io_uring` are still skipped on this host — expected,
documented Linux-only behavior (`CMAKE_SYSTEM_NAME STREQUAL "Linux"` guard),
unchanged from Phase 0. `cmake` was configured with `RTMP_SERVER_CORE_ONLY`
implied by the platform guard, exactly as in Phase 0.

## 11. Actual test result

Debug build: **201/201 tests passed** (190 pre-existing Phase 0 tests +
11 new Phase 1 integration tests), `ctest --test-dir build-debug
--output-on-failure`, total run time 1.73s.

ASan/UBSan build: **201/201 tests passed**, `ctest --test-dir build-asan
--output-on-failure`, total run time 8.73s.

One real regression was caught and fixed during this phase's own test
development, not silently worked around: the first version of
`RtmpFullSessionSocketIntegration.HandshakePlusConnectInSameWriteReachesTheSession`
failed (`got_result.load()` stayed `false`, actual output captured via
temporary debug instrumentation showed the client's decoder correctly
received all 202 reply bytes but never completed a message) because
`RtmpConnectionSession` used a 4096-byte encoder chunk size without ever
announcing it via `Set Chunk Size` — see section 2/4 above for the root
cause and fix (`RtmpConnectionSession::start()`). After the fix, the same
test passes deterministically.

## 12. Sanitizer result

AddressSanitizer + UndefinedBehaviorSanitizer (`-DRTMP_SERVER_ENABLE_ASAN=ON`,
per `cmake/Sanitizers.cmake`): **clean** — 201/201 tests passed with no
ASan or UBSan diagnostics of any kind, including the two real-socket
integration tests that exercise cross-thread ownership (test harness thread
writing into `RtmpConnectionSession`/`StreamRegistry` while the main test
thread reads `server.registry().is_published(...)` and `server.published()`
via `std::atomic`/completion-ordered access) and the abrupt-disconnect path
(closing a live socket mid-publish, then asserting on cleanup state).
`rtmp_server_io_uring`/`apps/rtmp_server` — where the real use-after-free
risk from stale kernel completions lives — could not be exercised under
ASan on this host, since that target does not build on macOS at all (Phase
0 finding, unchanged). This is the most significant caveat on the "No
use-after-free under ASan" definition-of-done bullet: it is true for
everything that built and ran, but the `io_uring`-specific completion/
generation-ID safety this phase's task 13 ("late kernel completion cannot
access destroyed session memory") is architecturally addressed
(`rtmp_sessions_` lives in `IoUringEventLoop`, keyed by `connection_id`,
looked up fresh on each use, never captured by raw pointer into
`io_uring` completion data — see `OperationContext`'s existing `weak_ptr<
TcpConnection>` pattern, unchanged and reused) but not sanitizer-verified
end-to-end on this host.

## 13. Performance observations

Not measured in this phase — no load or throughput claims are made.
Realistic load testing is explicitly Phase 7 scope. The only quantitative
observation is test wall-clock time (above), which is not a capacity claim.

## 14. Remaining risks

* **`io_uring` transport path is unbuilt/unverified on this host.** Every
  change to `event_loop.cpp`/`event_loop.hpp` (the actual production wiring)
  compiles only via the protocol-layer tests that link `rtmp_server_protocol`
  directly, plus manual code review — it has never been compiled by this
  session, because `RTMP_SERVER_CORE_ONLY`/the Linux guard prevents it on
  macOS. A Linux CI/dev machine must build `apps/rtmp_server` and
  `rtmp_server_io_uring` before this phase's `event_loop.cpp` changes can be
  considered verified, not just reviewed. This is the single largest gap
  against the Phase 1 Definition of Done bullets "OBS can connect and
  publish through the real server executable" and "A real RTMP client can
  connect and issue play" — neither was literally exercised against the
  compiled `rtmp_server` binary; both were exercised against the identical
  protocol-layer code path via `RtmpConnectionSession`, which the production
  binary reuses unchanged (no `#ifdef`/platform branching inside
  `RtmpConnectionSession` itself), but the `io_uring`-specific glue in
  `event_loop.cpp` (buffer pool lifetime, receive-handler installation
  timing relative to completion processing, cancellation interaction) is
  reviewed, not compiled or run, this session.
* **No dedicated "abrupt disconnect during play" test.** The disconnect
  cleanup code path (`RtmpConnectionSession::on_connection_closed` ->
  `CommandSession::on_connection_closed`) is identical regardless of
  whether the connection was publishing or playing at disconnect time, and
  `CommandSession`'s own pre-existing test suite
  (`ViewerDisconnectRemovesItFromFanoutSubscriberList`) already covers the
  viewer-disconnect-during-play case at the `CommandSession` level; this
  phase did not add an equivalent real-socket or `RtmpConnectionSession`-
  level test for that specific scenario. Low risk given the shared code
  path, but not literally proven at this layer.
* **`FCUnpublish` remains unhandled** (see section 3) — low risk (OBS/ffmpeg
  interoperate fine via `deleteStream`), but a client that relies solely on
  `FCUnpublish` without ever sending `deleteStream` would leave a stale
  publisher registration until connection close.
* **Process-wide, unsharded `StreamRegistry`/`LiveFanout`.** Correct for a
  single-worker event loop (today's reality — `worker_ring_count` is still
  unconsumed, per Phase 0 finding #9), but will need revisiting once Phase 4
  introduces multiple workers; `IoUringEventLoop` owning these by value
  means a naive multi-worker change would need explicit synchronization
  review (both types are already internally mutex-protected, so it is not
  unsafe today, just not yet sharded for throughput).

## 15. Breaking changes

None to any existing public interface — all changes are additive
(`take_trailing_bytes()`, `pending_write_bytes()`, the new
`RtmpConnectionSession` type, the new private `IoUringEventLoop` members).
`apps/rtmp_server` now links an additional library
(`rtmp_server_protocol`); this only affects the Linux-only build
configuration, which this session cannot build/test directly (see Remaining
risks).

## 16. Rollback considerations

Every change in this phase is additive at the type level and isolated to:
one new library source file, one CMake link-line addition, and a bounded
set of edits inside `IoUringEventLoop::start_handshake`/new private methods.
Reverting is a plain `git revert` of this phase's commit(s) with no data
migration, schema, or wire-format implications — nothing in this phase
changed any on-disk format, database schema, or the RTMP wire protocol
itself (the `Set Chunk Size` message is a pre-existing, spec-mandated RTMP
control message that was already implemented in `ChunkEncoder`; this phase
only added the missing call to send it).

## 17. Definition-of-done checklist

| Item | Status |
|---|---|
| OBS can connect and publish through the real server executable | **Not verified** — `apps/rtmp_server` cannot be built on this host (Linux-only); the identical protocol pipeline is verified via `RtmpConnectionSession` real-socket tests instead |
| A real RTMP client can connect and issue `play` | **Not verified** on the compiled binary, same caveat; verified at the `RtmpConnectionSession` layer (`CreateStreamPublishAndPlayFullLifecycle`) |
| Handshake trailing bytes are not lost | **Met** — `take_trailing_bytes()` + `HandshakeTrailingBytesArePreservedAndReachTheSession` + real-socket `HandshakePlusConnectInSameWriteReachesTheSession` |
| Socket input reaches the chunk decoder | **Met** at the architecture level (`event_loop.cpp` now installs `RtmpConnectionSession::on_bytes_received` as the post-handshake receive handler) and **verified** over a real loopback TCP socket via the test-harness substitute for `io_uring`; **not verified** through the actual `io_uring`-backed `TcpConnection` on this host |
| Encoded RTMP output reaches the socket | Same as above — met architecturally and via real-socket tests, not via the compiled `io_uring` binary |
| Connection cleanup is deterministic | **Met** — `cleanup_rtmp_session_state()` always runs from `close_connection()`; `AbruptDisconnectDuringPublishCleansUpRegistry` and `ConnectionCleanupUnregistersPublisherAndSubscriber` both pass |
| No use-after-free appears under ASan | **Met for everything built/run this session** (all 201 tests, including the real-socket ones); **not exercised** for the `io_uring`-specific completion path itself (see section 12/14) |
| All new integration tests pass | **Met** — 11/11 new tests pass under both Debug and ASan/UBSan |

Overall: **partially met**. The protocol/session-layer work (tasks 1-8,
10-15) is implemented, tested, and sanitizer-clean. Task 9 (control
messages) is implemented and tested, and its implementation directly fixed
a real interoperability bug caught by this phase's own tests. The
architectural wiring into `IoUringEventLoop` (task list items about the
actual executable) is implemented and reviewed but **not compiled or run on
this host**, because the `io_uring` transport is Linux-only — this is the
same, previously-documented platform constraint from Phase 0, not a new
limitation introduced here.

## 18. Recommended next phase

Per `docs/v2_promot.md`, the next phase is **PHASE 2 — Correct asynchronous
TCP transport**. Before starting it, it would materially de-risk this
phase's unverified items to get a Linux build/test run of
`apps/rtmp_server`/`rtmp_server_io_uring` (e.g. in CI or a Linux VM) against
this phase's `event_loop.cpp` changes specifically — confirming
`start_rtmp_session`/`cleanup_rtmp_session_state` compile and behave as
reviewed before Phase 2 builds further on top of the same file.
