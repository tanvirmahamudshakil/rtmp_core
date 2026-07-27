# Production readiness

Cross-check of the repository's **actual state** against every item in
`docs/v2_promot.md` section 6, "Final production acceptance criteria".

Produced at the end of Phase 8, the final phase. Written to be useful to
someone deciding whether to deploy this, which means it is deliberately
unflattering where the evidence is thin.

## How to read this

| Mark | Meaning |
|---|---|
| **Met** | Implemented, and verified by executed tests or an executed measurement on this host. |
| **Met (unverified here)** | Implemented and reviewed, but the verifying execution requires a Linux host with io_uring. The code exists and is reviewed; it has never run. |
| **Partially met** | Implemented in part, or implemented but not reachable in the deployable binary. |
| **Not met** | Absent. |

**The single most important thing in this document** is the wiring gap in
[Blocking gaps](#blocking-gaps): the primitives for authentication,
management, recording, HLS and persistence are all implemented and tested, but
**none of them are constructed by `apps/rtmp_server`**. The deployable binary
today is an unauthenticated RTMP relay. Everything else below should be read
with that in mind.

## Platform constraint

`src/io/io_uring/` and `apps/rtmp_server` are Linux-only and have **never been
compiled** in this repository's development environment (macOS). Every phase
report since Phase 4 records this. All local verification therefore uses the
`core-only` presets, which build the platform-independent subset: protocol,
media, recording, HLS, management, authentication, control, persistence,
observability, load generation.

This is why "Met (unverified here)" exists as a category, and it is a real
limitation, not a formality — roughly a third of the acceptance criteria below
concern code that has never executed anywhere in this project's history.

---

## Protocol

| Criterion | Status | Evidence |
|---|---|---|
| Complete RTMP handshake | **Met** | `HandshakeSession`, C0/C1/S0S1S2/C2 with trailing-byte capture. `tests/protocol/handshake/`, plus a real-socket integration test. |
| Correct chunk decoding and encoding | **Met** | `ChunkDecoder`/`ChunkEncoder`: basic header forms 1/2/3 byte, message header types 0-3 with inheritance, extended timestamps, interleaving. `tests/protocol/chunk/`. |
| AMF command processing | **Met** | `amf0::decode`/`decode_all`/`encode`; `CommandSession` handles connect/createStream/publish/play/deleteStream. |
| Publish and play lifecycle | **Met** | `tests/protocol/commands/command_session_test.cpp`, `tests/integration/rtmp_full_session_socket_test.cpp`. |
| RTMP control messages | **Met** | Set Chunk Size, Abort, Acknowledgement, Window Ack Size, Set Peer Bandwidth, User Control. |
| Ping/pong | **Met** | User Control PingRequest/PingResponse. |
| Safe malformed-input handling | **Met** | Every parser returns `core::Result`, never throws or aborts. 10.4M fuzz executions under ASan+UBSan across five harnesses; 469/469 tests clean under ASan+UBSan and TSan. Three remotely-triggerable resource-exhaustion defects found and fixed in Phase 8 (`docs/security.md`). |

## Connection lifecycle

| Criterion | Status | Evidence |
|---|---|---|
| Deterministic ownership | **Met (unverified here)** | `shared_ptr<TcpConnection>` owned by the event loop; sessions hold `weak_ptr`. Reviewed; the io_uring path has never run. |
| Partial send support | **Met (unverified here)** | `TcpConnection::async_write` queues and honours partial sends. Unit-tested at the queue level; the real socket path is Linux-only. |
| Bounded reads and writes | **Met** | Per-connection receive buffer, per-viewer queue bytes/packets, and (new in Phase 8) bounded chunk-reassembly state. |
| Correct timeout handling | **Met (unverified here)** | Handshake/idle/write/publisher-inactivity timeouts via io_uring linked timeouts. Never executed. |
| Safe shutdown | **Met (unverified here)** | `WorkerPool::stop()` drains on each worker's own thread. `docs/shutdown-model.md`. Never executed under a real signal. |
| No use-after-free | **Met** for core; **unverified** for transport | ASan clean over 469 tests, plus 10.4M fuzz executions. The io_uring completion paths — the most likely place for a stale-completion use-after-free — have never run under ASan. |
| No double close | **Met (unverified here)** | Reviewed. Phase 8 fixed a real fd-lifetime bug of exactly this class in `HttpServer` (which *is* testable here), which is some evidence the risk is real in the untested code too. |
| No stale completion access | **Met (unverified here)** | Generation counters on completion contexts. Reviewed only. |

## Streaming

| Criterion | Status | Evidence |
|---|---|---|
| Internal stream IDs | **Met** | `StreamIdRegistry` maps both the secret publish key and the public playback name to one `StreamId`. |
| Secure publish authentication | **Partially met** | `StreamManager::validate_publish_key` (hashed keys, constant-time compare) is implemented and tested. **Not constructed by `apps/rtmp_server`** — see Blocking gaps. |
| Secure playback authentication | **Partially met** | `verify_token` (HMAC-SHA256, time-boxed, constant-time) implemented and tested; `RtmpAuthenticator::playback_authorizer` implemented. **Not constructed by `apps/rtmp_server`.** |
| Shared immutable media payloads | **Met** | `SharedMediaFrame` holds `shared_ptr<const SharedBuffer>`; no per-viewer payload copy. |
| Correct GOP cache | **Met** | `GopCache` bounded by bytes/packets/duration, keyframe-aligned. `tests/protocol/commands/`. |
| Bounded viewer queues | **Met** | `ViewerQueue` bounded by bytes and packets. |
| Slow-viewer recovery and eviction | **Met** | Drop-to-keyframe then evict, driven by real pending-write bytes. |
| No callbacks while holding global fan-out locks | **Met** | `LiveFanout` copies subscriber handles under the lock and dispatches outside it. TSan clean. |

## Scalability

| Criterion | Status | Evidence |
|---|---|---|
| Multiple io_uring workers | **Met (unverified here)** | `WorkerPool` creates N `IoUringEventLoop`s, each with its own ring, bound via `SO_REUSEPORT`. Implemented in Phase 4. **Never executed** — the multi-worker behaviour has no execution evidence anywhere in this project. |
| Connection affinity | **Met (unverified here)** | A connection stays on the ring that accepted it. Reviewed only. |
| Egress worker sharding | **Met (unverified here)** | Per-worker fan-out with a cross-worker router. Reviewed only. |
| Bounded cross-worker queues | **Met (unverified here)** | `CrossWorkerRouter` uses bounded queues. Reviewed only. |
| No per-viewer cross-thread media copies | **Met** | Shared immutable payload; the queue holds a reference. Verifiable in core. |
| Realistic network load testing | **Partially met** | Phase 7 built a real load generator (`src/loadgen`, real TCP + real RTMP handshake and commands) and produced `docs/capacity-report.md`. But it was driven against `apps/rtmp_test_server`, a **test-only poll-based server**, not the io_uring server. The capacity numbers therefore characterise the protocol layer, **not the production transport**. |

## Operations

| Criterion | Status | Evidence |
|---|---|---|
| Metrics | **Partially met** | `observability::Metrics` + `/metrics` implemented and tested. Not constructed by `apps/rtmp_server`. |
| Structured logs | **Met** | `observability::Logger` emits structured JSON; used throughout, including by the io_uring layer. |
| Liveness | **Partially met** | `/health/live` implemented and tested; not wired into the deployable binary. |
| Readiness | **Partially met** | `/health/ready` implemented and tested; not wired into the deployable binary. |
| Management API | **Partially met** | Full CRUD, key rotation, playback tokens, disconnect, audit log — implemented and tested (`tests/control/`). Not wired into the deployable binary. |
| Graceful shutdown | **Met (unverified here)** | SIGTERM/SIGINT → `WorkerPool::stop()` → per-worker drain. Signal handler is async-signal-safe. Never executed under a real signal. |
| systemd deployment | **Partially met** | `deploy/systemd/rtmp-server.service` written and reviewed line by line; Phase 8 fixed a real defect in it (an invalid `SystemCallFilter` entry that would have caused startup failure under io_uring). **Never loaded by a running systemd**; `systemd-analyze verify` unavailable on this host. |
| Upgrade and rollback documentation | **Met** | `docs/deployment.md`. Written from the code's actual behaviour; not rehearsed end to end. |

## Evidence

| Criterion | Status | Evidence |
|---|---|---|
| Actual build logs | **Met** (core-only) | Clean build, zero first-party compiler warnings at the full strict warning set. |
| Actual test results | **Met** (core-only) | 469/469 passing across three configurations. |
| Sanitizer results | **Met** (core-only) | ASan+UBSan 469/469 clean; TSan 469/469 clean after fixing the race TSan found. |
| Load-test results | **Partially met** | Real, but against the test server, not the io_uring transport. |
| Capacity report | **Partially met** | `docs/capacity-report.md` — same caveat. |
| Known limitations | **Met** | This document. |

---

## Blocking gaps

These must be closed before this is a production system. They are ordered by
severity.

### 1. The deployable binary has no authentication, management, recording, HLS or persistence

**This is the gap that matters.**

`apps/rtmp_server/main.cpp` constructs exactly three things: a
`StreamRegistry`, a `StreamIdRegistry`, and a `WorkerPool`. It does not
construct `StreamManager`, `RtmpAuthenticator`, `HttpServer`, `ManagementApi`,
`SqliteStore`, `Recorder`, or any HLS component. `apps/rtmp_server`'s
`CMakeLists.txt` links only `rtmp_server_core`, `rtmp_server_protocol` and
`rtmp_server_io_uring` — the management, authentication, control, media and
persistence libraries are **not linked into the server executable at all**.

Consequently `IoUringEventLoop::start_rtmp_session` builds an
`RtmpConnectionSession` with `registry`, `stream_id_registry`, `live_fanout`
and queue limits — and **never sets `key_validator`, `stream_id_resolver` or
`playback_authorizer`**.

**Effect: the server as built accepts any publish key and any playback
request, with no authentication whatsoever.** It also records nothing, serves
no HLS, exposes no management API, no metrics and no health endpoints, and
persists nothing.

This is an improvement on the state the Phase 5/6 reports described — the RTMP
*session* wiring gap they flagged (`CommandSession` never constructed) **has
since been closed**; `event_loop.cpp:567` does construct a full
`RtmpConnectionSession`, so media ingest and fan-out are genuinely wired. But
the authorization and service-layer wiring was never done.

*Why it was not fixed in Phase 8:* `apps/rtmp_server` cannot be compiled on
this host. Writing several hundred lines of service-composition code into a
target that cannot be built, run or tested, and then reporting it as done, is
exactly what `docs/v2_promot.md` section 3.1 forbids. It is a focused,
well-understood task for a Linux host: link the libraries, construct the
components in `main()`, and thread the three authorization callbacks through
`WorkerPool` into `IoUringEventLoop`.

### 2. The io_uring transport has never been executed

Not once, in any phase. Multiple workers, `SO_REUSEPORT` distribution,
registered buffers, provided buffer rings, multishot accept/recv, linked
timeouts, cross-worker routing, and completion-lifetime handling are all
implemented and reviewed, and none of them have ever run.

Required before production: build on Linux, run the full suite there, run
ASan/UBSan and TSan there, and re-run the Phase 7 load matrix against
`apps/rtmp_server` rather than `apps/rtmp_test_server`.

### 3. Per-IP rate limiting is defeated by proxy TLS termination

`RtmpAuthenticator`'s per-IP connection cap and authentication-failure lockout
are keyed on client IP. The chosen TLS strategy terminates at a reverse proxy
(`docs/tls.md`), and the server does not parse the PROXY protocol — so every
connection appears to come from the proxy's address. Both mechanisms fail in
the worst way: the per-IP cap is consumed globally, and one attacker's failed
authentications lock out every legitimate client.

Worse, enabling `proxy_protocol on` against the current server *breaks
ingest*, because `HandshakeSession` consumes the PROXY header as a malformed
C0.

Until PROXY protocol parsing exists, enforce connection and rate limits at the
proxy and treat the server's per-IP limits as ineffective.

## Non-blocking gaps

- **No token revocation before expiry.** Tokens are stateless by design; only
  rotating `token_signing_secret` revokes, and that revokes *all* tokens at
  once with no overlap window (`docs/deployment.md` "Secret rotation").
- **No connection *rate* limit**, only a concurrency cap. An attacker can
  churn connections at high rate without exceeding the concurrent limit.
- **PostgreSQL persistence unimplemented.** `SqliteStore` only; single-node
  deployments only.
- **`clang-tidy` and `cppcheck` never run** — neither is installed on this
  host. Wired for CI via `-DRTMP_SERVER_ENABLE_CLANG_TIDY=ON`.
- **libFuzzer never used.** Apple Clang does not ship it. Fuzzing used the
  standalone mutation driver, which has no coverage feedback and so explores
  far less than libFuzzer would for the same execution count.
- **`amf0::require` assumes 64-bit `size_t`.** `offset + needed` cannot wrap
  on a 64-bit target; on a 32-bit target it could. Not a defect on any
  supported platform.
- **systemd, logrotate and Docker artefacts unexecuted.**
- **Capacity numbers do not describe the production transport** (gap 2).

## Summary

| Area | Assessment |
|---|---|
| Protocol correctness and malformed-input safety | **Strong.** Well tested, heavily fuzzed, sanitizer-clean, with real vulnerabilities found and fixed. |
| Core streaming architecture | **Strong.** Shared immutable payloads, bounded queues, no lock-held callbacks — all verified. |
| Security limits and hardening | **Strong** in the core; undermined in practice by gaps 1 and 3. |
| io_uring transport | **Unverified.** Reviewed, never executed. |
| Service composition | **Incomplete.** The deployable binary is missing everything above the protocol layer. |
| Operational documentation | **Complete**, and honest about what was never run. |

**Verdict: not production-ready.** The protocol and streaming core is in good
shape and the engineering behind it is sound. The obstacles are gap 1
(service composition — a focused, well-defined task) and gap 2 (the transport
has never run — an environment problem, not a code problem). Both need a Linux
host. Neither is a design flaw.

## Related

- `docs/phase-8-report.md` — Phase 8 evidence
- `docs/security.md` — security posture and findings
- `docs/deployment.md` — deployment procedures
- `docs/tls.md` — TLS strategy
- `docs/capacity-report.md` — Phase 7 load results and their caveats
