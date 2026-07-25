# Phase 0 — Repository Inspection and Design

## 1. Existing Repository Assessment

Repo state at inspection time:

```text
rtmp/
├── docs/
│   └── rtmp_promot.md   (spec doc, this file's source)
└── sample.tsxt          (empty, no content)
```

No source, no build files, no tests. Fresh repo. Nothing to preserve, nothing to delete. Proceed as greenfield init per rule "If the directory is empty, initialize a professional repository."

## 2. Proposed Architecture

Layered, per spec:

```text
Management API (HTTP/JSON)
        |
Stream Registry (auth/state/stats)
        |
   +----+----+
   |         |
Publisher  Subscriber sessions
   |         |
RTMP Protocol Engine (handshake/chunk/AMF0/commands) — no io_uring calls
        |
Async Transport Layer (io_uring only, via IAsyncTransport)
        |
   +----+----+
   |         |
FLV Recorder  Metrics/Logs
```

Hard rule: protocol/media/registry/auth code never calls liburing directly — only `io/io_uring/*` and `network/*` do, through `IAsyncTransport`.

## 3. Proposed File Tree

Adopt the structure defined in the spec (`docs/rtmp_promot.md` "Required Repository Structure") verbatim:

```text
rtmp/
├── CMakeLists.txt, CMakePresets.json, README.md, LICENSE, .gitignore, .clang-format, .clang-tidy
├── cmake/            (CompilerWarnings, Sanitizers, StaticAnalysis, Dependencies)
├── config/           (server.example.yaml, logging.example.yaml, environment.example)
├── docs/             (architecture, io-uring-design, connection-lifecycle, buffer-ownership,
│                      shutdown-model, rtmp-protocol, rtmp-handshake, chunk-parser, amf0,
│                      stream-lifecycle, timestamp-model, security, configuration,
│                      control-api, deployment, testing, troubleshooting, future-roadmap)
├── include/rtmp_server/  (core, io/io_uring, network, protocol/{handshake,chunk,amf0,commands,messages},
│                          media/{h264,aac,flv,gop}, server/{connection,publisher,subscriber,registry},
│                          authentication, recording, control, persistence, observability)
├── src/              (mirrors include/)
├── apps/             (rtmp_server, rtmp_probe, rtmp_client_test, flv_inspector)
├── tests/            (unit, integration, protocol, load, fuzz)
├── scripts/          (build-debug.sh, build-release.sh, run-tests.sh, run-sanitizers.sh, load-test.sh)
└── deploy/           (docker, systemd, logrotate)
```

## 4. Module Boundaries

- **core**: error types, Result<T>, byte helpers, clocks, IDs, RAII fd, signal handling — no networking, no protocol knowledge.
- **io/io_uring**: ring lifecycle, capability detection, event loop, operation contexts. Only place `liburing.h` is included.
- **network**: `IAsyncTransport`, `AsyncTcpConnection`, `AsyncTcpAcceptor` — wraps io_uring behind the abstract interface consumed by everything above it.
- **protocol**: handshake, chunk parser/encoder, AMF0, command handling. Pure state machines operating on buffers; no sockets, no io_uring.
- **media**: H.264/AAC packet inspection, FLV encoding, GOP cache. Pure data transforms, no I/O.
- **server**: connection/publisher/subscriber sessions, stream registry — orchestrates protocol + media + transport via interfaces.
- **authentication**: token signing/validation, key generation, throttling.
- **recording**: FLV writer + async file I/O via io_uring file-write ops (still behind transport-layer abstraction).
- **control**: HTTP management API.
- **persistence**: SQLite (dev) / PostgreSQL (prod) behind a repository interface, plus in-memory authorization cache.
- **observability**: structured logging, metrics.

## 5. Connection Ownership

- Each `Connection` is owned by exactly one event-loop (`IoUringEventLoop` instance / worker ring). No cross-thread mutation of connection state.
- Identity: `(connectionId, generation)` pair, never raw fd or raw pointer, since fds are reused by the OS.
- Cross-thread commands (e.g., API-triggered disconnect) go through a bounded queue + eventfd wakeup consumed by the owning loop, resolved by connection ID.

## 6. Operation Ownership

- Every submitted io_uring SQE carries a heap-stable `OperationContext { type, operationId, connectionId, generation, weak_ptr<Connection> }` referenced via `user_data`.
- Completions are matched by `operationId`; stale completions (generation mismatch, connection already closed) are logged at debug level and discarded, never treated as fatal.
- No raw pointers in `user_data` without a formally guaranteed lifetime — always via `OperationRegistry`.

## 7. Buffer Ownership

- `SharedBuffer` (ref-counted, immutable payload) is the unit passed between protocol/media/transport layers — avoids copies on fan-out to N subscribers.
- `BufferPool` / `RegisteredBufferPool` / `ProvidedBufferRing` own raw memory; a buffer is never recycled into the pool before its in-flight io_uring operation completes.
- GOP cache and subscriber queues hold `shared_ptr<const MediaPacket>` wrapping `SharedBuffer` — bounded by byte/packet/duration limits, not open-ended.

## 8. Threading Model

- Phase 1 milestone: single io_uring event-loop thread (correctness first).
- Later: configurable `worker_ring_count`, one accept loop distributing accepted connections round-robin (or by hash) to worker rings. Each worker ring is a fully independent io_uring instance — no shared ring, no lock-free cross-ring structures unless proven necessary and benchmarked.

## 9. Shutdown Model

Per spec's Graceful Shutdown section (SIGTERM/SIGINT): stop accept → reject new API mutations → close publishers/viewers → stop new recordings → flush recording queues → cancel timeouts/receives → complete/cancel sends → drain CQEs → close sockets → finalize recordings → flush logs → close DB → destroy rings → exit. Configurable deadline; never destroy an `OperationContext` while a completion for it can still arrive.

## 10. Timestamp Model

Central `TimestampNormalizer` component (not ad hoc per-callsite math) responsible for: RTMP 32-bit timestamp + extended timestamp combination, wraparound, DTS/PTS via composition offset, discontinuity/reset detection on publisher reconnect, and subscriber-relative rebasing on join. Documented separately in `docs/timestamp-model.md` before media routing is implemented (Phase 5+).

## 11. Security Boundaries

- Parser layers (handshake, chunk, AMF0, H.264/AAC inspection) are the untrusted-input boundary: every length/count is bounds-checked before use; AMF0 enforces max string/nesting/property/array limits.
- Auth boundary: stream keys/tokens compared in constant time, never logged; API bound to `127.0.0.1:8080` by default.
- Resource boundary: per-IP and total connection limits, bounded queues/buffers everywhere, so a malformed or hostile client cannot exhaust memory or crash the process.

## 12. Risks

- io_uring feature availability varies by kernel version — capability detection must degrade to correct io_uring-native fallbacks (never epoll) and this needs early validation on the actual target kernel.
- Getting `OperationContext`/generation handling wrong is the highest-risk correctness area (use-after-free / stale completion bugs) — must be covered by dedicated integration tests before RTMP layers are built on top.
- Backpressure policy (slow subscriber handling) has real product-behavior implications (frame drops) — needs explicit documented policy, not implicit.

## 13. Implementation Order (Phases)

Matches spec's Implementation Phases section exactly:

0. Repository Inspection and Design (this document)
1. Core and io_uring TCP Foundation — first executable milestone
2. RTMP Simple Handshake
3. RTMP Chunk Engine
4. AMF0 and RTMP Commands
5. Media Ingest (H.264/AAC/metadata)
6. FLV Recording
7. RTMP Playback (fan-out, GOP cache, backpressure)
8. Management API and Link Generation
9. Persistence and Production Hardening

Compile + test after every phase; no phase is "done" without an actual passing build and actual passing tests.
