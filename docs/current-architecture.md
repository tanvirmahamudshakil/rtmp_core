# Current Architecture (v2 audit, verified 2026-07-25)

This document describes what the repository **actually does today**, verified by reading
source and by building/running it. It supersedes any optimistic claims in
`docs/architecture.md` or `docs/phase*-checklist.md` from the prior (v1) round where they
conflict with this audit.

## 1. What actually runs

The only executable that starts a network server is `apps/rtmp_server` (`apps/rtmp_server/main.cpp`).
It:

1. Loads `ServerConfig` from YAML (`rtmp_server::core::load_config`).
2. Creates one `IoUringContext` + one `IoUringEventLoop` (`src/io/io_uring/event_loop.cpp`).
3. Installs `SIGINT`/`SIGTERM` handlers that call `loop.stop()`, ignores `SIGPIPE`.
4. Calls `loop.run()`, which blocks until shutdown.

`apps/rtmp_server/CMakeLists.txt` links only:

```
rtmp_server_core
rtmp_server_io_uring
rtmp_server_warnings
rtmp_server_sanitizers
```

It does **not** link `rtmp_server_protocol`, `rtmp_server_management`, `rtmp_server_media`,
`rtmp_server_persistence`, or any HTTP library. There is no HTTP server target anywhere in
the repository at all (`grep -rli http include src apps` matches only the string "http" in a
couple of header/CMake comments, never an implementation).

## 2. Real runtime path, traced from source

```
main()                                   apps/rtmp_server/main.cpp
  -> load_config()                       src/core/config.cpp
  -> IoUringContext::create()            src/io/io_uring/context.cpp
  -> IoUringEventLoop::run()             src/io/io_uring/event_loop.cpp:27
       -> socket()/bind()/listen()
       -> submit_accept()                event_loop.cpp:126
       -> io_uring_submit_and_wait() loop, single OS thread
       -> process_completion() dispatches by OperationType
            Accept   -> handle_accept_completion()   event_loop.cpp:316
            Receive  -> handle_receive_completion()  event_loop.cpp:449
            Send     -> handle_send_completion()     event_loop.cpp:492
            Timeout  -> handle_timeout_completion()  event_loop.cpp:504
```

On accept (`event_loop.cpp:316-354`):
* A `TcpConnection` is created and added to `ConnectionRegistry`.
* `start_handshake()` builds one `HandshakeSession` and sets
  `connection->set_receive_handler(...)` to a lambda that forwards every received byte span
  straight into `HandshakeSession::on_bytes_received` (`event_loop.cpp:387-389`).
* A handshake timeout is armed.

`HandshakeSession` (`src/protocol/handshake/handshake_session.cpp`) consumes C0, C1, C2 in
sequence. On completion it calls `complete_handler_`, which (`event_loop.cpp:364-375`):
* Sets connection state to `Connected`.
* Removes the handshake session from `handshake_sessions_`.
* Re-arms the connection's **idle** timeout.
* Does **nothing else** — the receive handler installed at accept time is never replaced.

**This is the actual end of the runtime path.** Every byte the peer sends after the
handshake completes still invokes the same lambda that calls
`HandshakeSession::on_bytes_received`, but that method returns immediately once the session
is in a terminal state (`is_terminal()` guard, `handshake_session.cpp:42`). The bytes are
read off the socket (so the peer is not blocked), acknowledged to the kernel, and then
silently discarded. `ChunkDecoder`, `ChunkEncoder`, `CommandSession`, `MediaIngest`,
`StreamRegistry`, and `LiveFanout` are fully implemented, unit-tested, and built into
`rtmp_server_protocol`/`rtmp_server_management`, but **no file under `src/io`, `src/network`,
or `apps/rtmp_server` references any of those five class names** (verified by
`grep -rl "ChunkDecoder\|ChunkEncoder\|CommandSession\|MediaIngest\|LiveFanout\|StreamRegistry" src/io src/network apps/rtmp_server` -> zero matches).

Net effect: a real RTMP client (OBS or otherwise) can complete the handshake against this
server and nothing further will ever happen — no `connect` result, no publish acknowledgement,
no playback — because the executable that would need to wire the chunk/command layer to the
socket does not link that layer at all, and even if it did, the receive handler is never
swapped out after the handshake.

## 3. Executables

| Target | Path | Platform | Purpose |
|---|---|---|---|
| `rtmp_server` | `apps/rtmp_server` | Linux only (guarded by `CMAKE_SYSTEM_NAME STREQUAL "Linux"`) | The only network-facing binary. Handshake-only in practice (see above). |
| `flv_inspector` | `apps/flv_inspector` | all | Offline FLV file reader/dumper, no sockets. |
| `load_bench` | `apps/load_bench` | all | In-process synthetic benchmark, no sockets (see section 6). |
| `fuzz_amf0_decoder`, `fuzz_chunk_decoder`, `fuzz_flv_parser` | `fuzz/` | all | libFuzzer-compatible corpus-replay/fuzz targets for the three parsers. No handshake/AMF-command fuzz target exists. |

## 4. Libraries / CMake target graph

```
rtmp_server_core          (src/core)            - Result/Error, Config, HMAC, secure random, buffers, logger, metrics, audit log
rtmp_server_protocol      (src/protocol)        - AMF0 codec, chunk decoder/encoder, handshake session,
                                                   CommandSession, LiveFanout, StreamRegistry, MediaIngest
rtmp_server_media         (src/media)           - FLV writer + Recorder orchestration (recording disk I/O lives here)
rtmp_server_persistence   (src/persistence)     - SQLite-backed store (system libsqlite3, no io_uring dep)
rtmp_server_management    (src/management)      - StreamManager, Token (sign/verify), AuthorizationCache, UrlBuilder
                                                   -> links rtmp_server_protocol + rtmp_server_persistence
rtmp_server_io_uring      (src/io/io_uring)     - IoUringContext, IoUringEventLoop, capabilities probing, file sink
                                                   Linux-only; does NOT link rtmp_server_protocol or
                                                   rtmp_server_management
```

Top-level `CMakeLists.txt` always configures `core`, `protocol`, `media`, `persistence`,
`management`, `flv_inspector`, `load_bench` on every platform; `io_uring` and `rtmp_server`
are added only `if(NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux")`.

**Unlinked management modules (confirmed):** `rtmp_server_management` and
`rtmp_server_protocol` build successfully as libraries and are fully covered by tests, but
`apps/rtmp_server/CMakeLists.txt` links neither of them into the actual server executable.

## 5. RTMP protocol components (all exist, all unit-tested, none wired to the real executable)

* `src/protocol/handshake/handshake_session.cpp` — simple handshake (C0/C1/C2 <-> S0/S1/S2), size-bounded input buffer (`kMaxHandshakeBytes`), does not verify C2 content (documented, deliberate).
* `src/protocol/chunk/chunk_decoder.cpp`, `chunk_encoder.cpp` — full chunk-stream reassembly/encoding to/from `RtmpMessage`.
* `src/protocol/amf0/*` — AMF0 encode/decode.
* `src/protocol/commands/command_session.cpp` — `connect`/`createStream`/`publish`/`play`/`deleteStream`/`FCPublish` command handling, per-connection `StreamSlot` table, playback relay wiring to `LiveFanout`.
* `src/protocol/commands/stream_registry.hpp` — single-publisher-per-key registry, keyed by a plain `std::string` (no `StreamId`/`ApplicationId` strong type exists anywhere in the repo — verified by `grep -rn "struct StreamId\|struct ApplicationId\|struct PublisherId\|struct SubscriberId" include src` -> zero matches).
* `src/protocol/commands/live_fanout.cpp` — GOP cache + subscriber fan-out, keyed by the same raw string.
* `src/protocol/media/media_ingest.cpp` — turns decoded `RtmpMessage`s into audio/video/metadata callbacks.

## 6. Networking components

* `src/network/tcp_connection.cpp` / `include/rtmp_server/network/tcp_connection.hpp` — per-connection state machine, owns an unbounded `std::deque<core::SharedBuffer> write_queue_`.
* `src/io/io_uring/event_loop.cpp` — the only event loop; single OS thread; owns the listener socket, `BufferPool`, `OperationRegistry`, connection registry, handshake-session map.
* `src/io/io_uring/context.cpp`, `capabilities.cpp` — ring setup and kernel feature probing (multishot accept/recv, provided buffers) — probed but the probed capabilities are not consulted anywhere else in the codebase to change behavior (`capabilities()` accessor exists on `IoUringEventLoop` but has zero callers outside its own header/tests).
* `src/io/io_uring/file_sink.cpp` — async file writes via io_uring (used by recording, Linux only).

## 7. Persistence

* `src/persistence/sqlite_store.cpp` — schema creation, applications/streams tables, upsert/delete, transactions. Fully tested (`tests/persistence/sqlite_store_test.cpp`, 7 tests, all pass). Not reachable from the running server (nothing wires `StreamManager`/`SqliteStore` into `apps/rtmp_server`).

## 8. Management / control plane

* `src/management/stream_manager.cpp` — application/stream CRUD, publish-key hashing + rotation, playback token issuance, live-state queries, publisher/viewer disconnect API, audit logging. Fully unit-tested against an in-memory fan-out/registry pair, never constructed by `apps/rtmp_server`.
* `src/management/token.cpp` — HMAC-signed playback tokens with expiry; `management::Token::validate`-equivalent logic verified in isolation (`tests/management/token_test.cpp`), never called from `CommandSession::handle_play` (`src/protocol/commands/command_session.cpp:275-293` takes only a raw stream-key string argument and performs no token lookup or validation of any kind).
* `src/management/authorization_cache.cpp` — TTL cache in front of an authorization loader; exists, tested, unused by the real server.
* No HTTP server, no `/v1/...` routes, no `/metrics`, `/health/live`, `/health/ready` endpoints exist anywhere in the repository.

## 9. Media

* `src/media/flv/flv_writer.cpp` — FLV tag serialization.
* `src/recording/recorder.cpp` — bounded recording queue, disk-failure handling, finalize/idempotency, tested via `tests/recording/recorder_test.cpp` (feeds it in-memory, not through the real server).

## 10. Tests

`ctest --test-dir build-debug` (Debug, macOS/Darwin, `RTMP_SERVER_CORE_ONLY` implied by the platform guard since `io_uring`/`rtmp_server` are skipped): **190/190 tests pass**. Test suites: unit (core), protocol (handshake/chunk/AMF0/command_session, including a socket-pair-based handshake integration test), media (FLV writer), recording, management (stream manager, token, url builder, authorization cache, persistence-backed stream manager), persistence (SQLite store).

There is **no integration test that drives a full accept -> handshake -> publish -> play ->
media -> disconnect sequence through the real `IoUringEventLoop`**, because the event loop
does not wire up that far (section 2). The existing "integration" tests
(`tests/protocol/handshake/handshake_socket_integration_test.cpp`) exercise `HandshakeSession`
over a real socket pair, not the full pipeline.

## 11. Benchmarks

`apps/load_bench/main.cpp` (140 lines): in-process only, no TCP sockets at all. It builds
`RtmpMessage`s with hand-crafted 1-2 byte payloads (e.g. `{0x17,0x01}` for a keyframe,
`apps/load_bench/main.cpp:126`) and pushes them directly through `LiveFanout` in memory
across N synthetic subscribers. A sample run:

```
$ ./build-debug/apps/load_bench/load_bench
load_bench: streams=4 viewers_per_stream=50 frames=1000
delivered 200000 viewer messages in 0.086s (2331886 messages/sec)
```

This measures **only** the in-memory dispatch loop of `LiveFanout::dispatch_locked` (a
`std::unordered_map` iteration + virtual call per subscriber) — it does not exercise
`io_uring`, TCP, chunk encoding, partial sends, or realistic H.264/AAC frame sizes (typical
keyframes are tens of KB, not 2 bytes). "2.3M messages/sec" is not a claim about network
throughput or concurrent-viewer capacity of the real server.

## 12. Deployment files

`deploy/docker/Dockerfile`, `deploy/systemd/rtmp-server.service`, `deploy/logrotate/*` exist
but describe running the `rtmp_server` binary, which — per section 2 — cannot yet serve real
publish/play traffic. They are not evaluated further in this phase (Phase 8 concern).

## 13. Threading model (verified)

`grep -rn "worker_ring_count\|std::thread" src/io/io_uring apps/rtmp_server` returns zero
matches outside the config struct itself. Exactly one `IoUringEventLoop` runs, on the thread
that calls `main()`. `ServerConfig::worker_ring_count` (`config.hpp:24`, default `1`) is parsed
from YAML/env but never read by any code that creates a second ring or thread.
