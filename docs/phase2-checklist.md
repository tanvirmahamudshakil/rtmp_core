# Phase 2 Implementation Checklist — RTMP Handshake

- [x] C0/C1/C2 parsing — `HandshakeSession::try_consume_c0/c1/c2`
- [x] S0/S1/S2 generation — `HandshakeSession::try_consume_c1` (builds and sends the response)
- [x] Handshake state machine — `HandshakeState` (`WaitingForC0`, `WaitingForC1`,
  `SendingS0S1S2`, `WaitingForC2`, `Completed`, `Failed`, `TimedOut`)
- [x] Fragmented input — byte-accumulation buffer, consumed incrementally; covered by
  `FragmentedC0C1SucceedsByteAtATime`, `C0AndC1DeliveredTogetherThenSplitAcrossReceives`,
  `FragmentedC2SucceedsInSmallPieces`, and the real-socket integration test
- [x] Partial output — reuses Phase 1's `TcpConnection`/`IoUringEventLoop` ordered
  send queue and partial-send resubmission as-is; no new send machinery added
- [x] Timeout — new `TimeoutPurpose::Handshake` timeout armed on accept and re-armed
  on every partial receive while handshaking (`IoUringEventLoop::arm_handshake_timeout`),
  wired to `HandshakeSession::on_timeout()`
- [x] Invalid-version handling — `HandshakeState::Failed` with `ErrorCode::MalformedHandshake`,
  connection closed cleanly, no crash
- [x] Tests — 12 new GoogleTest cases (10 unit + 2 real-socket integration), see below

## Files created

- `include/rtmp_server/protocol/handshake/handshake_session.hpp`
- `src/protocol/handshake/handshake_session.cpp`
- `src/protocol/CMakeLists.txt`
- `tests/protocol/handshake/handshake_session_test.cpp`
- `tests/protocol/handshake/handshake_socket_integration_test.cpp`
- `tests/protocol/CMakeLists.txt`
- `docs/phase2-checklist.md` (this file)

## Files changed

- `CMakeLists.txt` — added `add_subdirectory(src/protocol)` (unconditional, platform-independent)
  and `add_subdirectory(tests/protocol)` (under `RTMP_SERVER_BUILD_TESTS`)
- `src/io/io_uring/CMakeLists.txt` — links `rtmp_server_protocol` into `rtmp_server_io_uring`
- `include/rtmp_server/io/io_uring/event_loop.hpp` — added handshake wiring members/methods
  (`start_handshake`, `arm_handshake_timeout`, `cleanup_handshake_state`,
  `handshake_sessions_`, `handshake_timeout_operation_ids_`)
- `src/io/io_uring/event_loop.cpp` — `handle_accept_completion` now starts a
  `HandshakeSession` per connection instead of just arming the idle timeout;
  `handle_receive_completion` re-arms the handshake timeout instead of the idle
  timeout while a handshake is in flight; `handle_timeout_completion` handles
  `TimeoutPurpose::Handshake`; `close_connection` cleans up handshake state
- `apps/rtmp_server/main.cpp` — comment pointing at the actual wiring location
  (see "Architecture decisions" in the Phase 2 report for why the wiring itself
  lives in `event_loop.cpp`, not `main.cpp`)
- `docs/rtmp-handshake.md` — replaced stub with full documentation

## Build and test evidence

See the Phase 2 report delivered alongside this checklist for verbatim
`cmake`/`ctest` command output. Summary: `core-only` preset (this build host is
macOS/Darwin — no `io_uring`) builds clean, 28/28 tests pass (16 pre-existing
Phase 0/1 core tests + 12 new Phase 2 protocol tests), and the same 28 tests
pass clean under `-fsanitize=address,undefined` on the `asan` preset
configured with `RTMP_SERVER_CORE_ONLY=ON`.

## Known limitations

- The `io_uring`-backed wiring (`event_loop.cpp` changes) could not be
  compiled or run in this environment — no Linux host with `liburing`
  available. It was reviewed carefully against Phase 1's existing patterns
  (operation contexts, generation-based identity, map-based ancillary state
  keyed by `connection_id`, cancel-then-rearm for timeouts) but is
  **unverified by an actual build** here. The protocol-layer code
  (`HandshakeSession` itself, which is the actual RTMP logic) is fully
  built, tested, and sanitizer-clean.
- No fake/mock clock exists in `core/clock.hpp`, so the *real* elapsed-time
  firing of an `io_uring` handshake timeout is not exercised by an
  automated test — only the `on_timeout()` code path is (see
  `docs/rtmp-handshake.md` "Known limitations" for the full reasoning).
- No real OBS instance was available; substituted with a scripted
  real-loopback-socket test performing a byte-correct handshake.

Next: Phase 3 — RTMP Chunk Engine.
