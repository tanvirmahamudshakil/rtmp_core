# Security

## Scope

Consolidated security posture across the codebase, plus the Phase 9
"security review findings are documented" acceptance criterion. Per-area
detail lives in each area's own doc (`docs/rtmp-handshake.md`,
`docs/control-api.md` "API security", `docs/phase*-checklist.md` "Security
concerns" sections); this doc is the index and the place new cross-cutting
findings/decisions get recorded going forward.

## Cryptography

- All randomness (stream keys, tokens, connection/operation IDs) goes through
  `core::secure_random_bytes` (OpenSSL `RAND_bytes`) — never `std::rand`,
  never a non-cryptographic PRNG (`core/random.hpp`).
- All hashing/signing goes through `core::hmac_sha256_hex`/`sha256_hex`
  (OpenSSL EVP) — no hand-rolled cryptography anywhere in the codebase
  (`core/hmac.hpp`, added Phase 8).
- All secret comparisons (stream-key hash match, token signature
  verification, and — once a management API secret check is wired in — the
  `api_authentication_secret` check) go through `core::constant_time_equals`
  (OpenSSL `CRYPTO_memcmp`), never `std::string::operator==`.

## Authentication and authorization

- **Publish**: `protocol::commands::StreamKeyValidator`, a pluggable
  `bool(app, key)` hook on `CommandSession` (Phase 4) — the real
  implementation is `management::StreamManager::validate_publish_key`
  (Phase 8), gated by both application-level and stream-level `enabled`
  flags, key-hash comparison only (raw keys are never persisted — see
  `docs/control-api.md` "Domain model").
- **Playback**: `management::verify_token`/`StreamManager::
  verify_playback_token` (Phase 8) — stateless HMAC-signed, time-boxed
  tokens; not yet consulted by `CommandSession::handle_play` (see
  `docs/rtmp-playback.md` and `docs/control-api.md` "Known limitations" —
  this is a wiring gap, not a missing primitive).
- **Management API**: `ServerConfig::api_authentication_secret` is declared
  and validated non-default at startup, but — since no HTTP server exists
  yet (`docs/control-api.md` "What this phase deliberately does not do") —
  there is no request path that actually checks it yet. Whatever HTTP layer
  gets built on top of `StreamManager` must compare it with
  `core::constant_time_equals`, same as everything else in this list.
- **No key/token enumeration**: `validate_publish_key` returns the same
  `false` for "unknown key", "disabled stream", and "disabled application" —
  an attacker probing keys learns nothing from the response shape.
- **Authorization cache** (`management::AuthorizationCache`, Phase 9):
  caches both positive and negative `validate_publish_key` results for a
  bounded TTL, so a probing attacker sending many rapid publish attempts
  against unknown keys doesn't force a hash computation + lookup per
  attempt — the negative-result cache absorbs it. Not itself a rate limiter
  (no per-source-IP tracking) — see "Known limitations" below.

## Input handling

Every parser that consumes attacker-controlled bytes rejects malformed input
via `core::Result` rather than crashing or reading out of bounds — verified
under ASan/UBSan for every phase (see each `docs/phase*-checklist.md`
"Sanitizer results") and, starting Phase 9, additionally exercised by
fuzzing:

- `protocol::chunk::ChunkDecoder` — RTMP chunk stream, the first parser
  attacker bytes reach post-handshake (`fuzz/fuzz_chunk_decoder.cpp`).
- `protocol::amf0::decode`/`decode_all` — AMF0 command/data payloads
  (`fuzz/fuzz_amf0_decoder.cpp`).
- `media::flv::parse_flv` — untrusted `.flv` file input, e.g. a corrupted
  recording or a file handed to `apps/flv_inspector`
  (`fuzz/fuzz_flv_parser.cpp`).

See `docs/testing.md` "Fuzzing" for how to actually run these (requires
Clang + libFuzzer; not available on this project's macOS development host —
see Known limitations).

## Logging and audit

- `observability::Logger` deliberately never logs full tokens/stream
  keys/secrets — callers must pre-redact (documented in
  `include/rtmp_server/observability/logger.hpp`).
- `observability::AuditLog` (Phase 9) records every management-API mutation
  (create/delete/rotate/enable/disable/disconnect) with outcome, but never a
  raw key or token (`AuditEntry::detail` is free-form but every call site in
  `StreamManager` only ever puts static strings like `"store write failed"`
  into it, never secret-derived data).

## Process/deployment hardening

- `deploy/systemd/rtmp-server.service` (Phase 9): runs as a dedicated
  unprivileged user, `ProtectSystem=strict`, `NoNewPrivileges`,
  capability-bounded to `CAP_NET_BIND_SERVICE`, restrictive
  `SystemCallFilter`. Not verified on this host (no systemd) — see
  `docs/deployment.md`.
- `deploy/docker/Dockerfile` (Phase 9): runtime image runs as a non-root
  user, minimal installed packages (no build toolchain in the runtime
  stage). Not verified on this host (no Docker) — see `docs/deployment.md`.

## Static analysis

`.clang-tidy` (checks: `bugprone-*`, `performance-*`, `modernize-*`,
`cppcoreguidelines-*`) and `cmake/StaticAnalysis.cmake`
(`-DRTMP_SERVER_ENABLE_CLANG_TIDY=ON`) have existed since Phase 0 and apply
to every header under `include/rtmp_server/` (`HeaderFilterRegex`),
including everything added in Phases 8-9. **Not run in this environment**:
`clang-tidy` is not installed on this project's macOS development host (no
Homebrew LLVM, no Xcode-bundled `clang-tidy`) — see
`docs/phase9-checklist.md` "Known limitations". A CI environment with
`clang-tidy` available should run `cmake --preset core-only
-DRTMP_SERVER_ENABLE_CLANG_TIDY=ON` before merging.

> **Note (v2 Phase 8).** The section below predates the v2 phase sequence
> (`docs/v2_promot.md`) and several of its entries are now out of date.
> `docs/v2_promot.md` PHASE 8 supersedes it; see
> "v2 Phase 8 — security hardening" at the end of this document for the
> current position on each item, and `docs/production-readiness.md` for the
> authoritative list of open gaps.

## Known limitations (security review findings, pre-v2 — see note above)

- **No rate limiting / failed-authentication throttling** anywhere (publish
  attempts, management API calls). `AuthorizationCache`'s negative-result
  caching reduces load from repeated identical probes but does not bound the
  rate of *distinct* key guesses.
- **No IP restrictions** on publish/playback (listed as optional in
  docs/rtmp_promot.md "Authentication", not required by any phase's
  acceptance criteria so far).
- **No single-token revocation** — see `docs/phase8-checklist.md`
  "Architecture decisions" (stateless tokens trade this off deliberately;
  only whole-secret rotation revokes all outstanding tokens at once).
- **`validate_publish_key`/`verify_playback_token` are not wired into any
  running `CommandSession`** — see `docs/control-api.md` "Known
  limitations". The primitives are implemented and tested; the integration
  point (a live session-owner layer) doesn't exist yet on this host.
- **PostgreSQL persistence is unimplemented** — see `docs/phase9-checklist.md`.
  `SqliteStore` has no row-level access control beyond what the single
  process using it enforces in-memory (`StreamManager`'s mutex); this is
  fine for the single-node deployment model this phase targets.
- **Fuzzing harnesses cannot run with libFuzzer on this host** (Apple
  Clang lacks the libFuzzer runtime) — see `docs/testing.md` "Fuzzing".
  They do build and run as standalone corpus-replay tools, and were
  smoke-tested that way, but have not accumulated real fuzzing time/coverage
  as part of this phase.
- **`clang-tidy` has not been run** — see "Static analysis" above.
- **Docker/systemd deployment are unverified** — see "Process/deployment
  hardening" above and `docs/deployment.md`.


---

# v2 Phase 8 — security hardening

This section is the current security position. Where it contradicts an
earlier section, this one is correct. Full evidence in
`docs/phase-8-report.md`; open gaps in `docs/production-readiness.md`.

## Vulnerabilities found and fixed in Phase 8

All four were found by audit or fuzzing, reproduced with an executed
proof-of-concept against the pre-fix code, fixed, and locked down with
regression tests.

### 1. AMF0 unbounded recursion — remote unauthenticated crash

**Severity: critical.** `amf0::decode_value` and `decode_properties` recursed
into each other with no depth bound. Nested AMF0 objects therefore mapped
directly onto call-stack depth.

*Reproduced:* 800 001 bytes of `03 00 01 61` repeated (object → one-character
property name → next object) terminated the process with SIGSEGV (exit 139) —
a stack overflow.

*Reachability:* any peer that completes the RTMP handshake can send an AMF0
command message. No authentication is required, so this was a pre-auth remote
denial of service against the whole process (and every other connection it
was serving).

*Fix:* `amf0::kMaxNestingDepth = 32`, threaded through both functions and
applied to strict arrays as well as objects/ECMA arrays. Real command objects
nest 2-3 levels; the deepest structure the server itself emits is 3.

### 2. AMF0 strict-array unchecked allocation

**Severity: high.** The 32-bit element count of a strict array was passed
straight to `vector::reserve`. Five bytes of input (`0A FF FF FF FF`)
requested storage for 4 294 967 295 `Amf0Value`s — roughly 275 GB of address
space.

*Fix:* the count is validated against the bytes actually remaining before any
allocation. Every element needs at least one byte on the wire, so a larger
count is provably a lie and is rejected without allocating.

### 3. Chunk-stream reassembly amplification

**Severity: high.** `ChunkDecoder` created a `ChunkStreamState` per chunk
stream ID on demand — the basic header can address 65 600 of them — and
immediately called `partial_payload.reserve(declared_message_length)`, where
the declared length was validated only against `max_message_size` (10 MiB).

*Reproduced:* 2 840 000 bytes of input, spread over 20 000 chunk stream IDs
each declaring a 10 MiB message and sending one default-size chunk, drove a
single decoder to **269 MiB RSS / 792 MiB peak footprint**. That is ~95x
resident amplification, per connection, before authentication.

*Fix:* `ChunkDecoderLimits` — a cap on concurrently tracked chunk stream IDs
(64; real publishers use 3-5), a cap on total outstanding reassembly bytes
across the connection, a spec-conformant `0xFFFFFF` cap on negotiated chunk
size (the wire field allows up to `0x7FFFFFFF`), and a bounded initial reserve
so memory tracks bytes actually received rather than bytes merely claimed.
The same input now fails cleanly at **11 MiB peak**.

### 4. `parse_flv` out-of-bounds read

**Severity: medium.** Found by `fuzz/fuzz_flv_parser` under ASan
(container-overflow in `core::read_u32_be`). `DataOffset` was validated as
`<= data.size()`, but `PreviousTagSize0` is read as four bytes starting *at*
that offset — so any `DataOffset` in `[size-3, size]` passed validation and
then read past the end of the buffer.

*Fix:* the bound now includes the room `PreviousTagSize0` needs, written as a
subtraction so a `DataOffset` near 2^32 cannot wrap the addition.

### 5. `HttpServer` accept/stop data race (found by TSan)

**Severity: medium.** `listen_fd_` was a plain `int`, written by `stop()` on
the caller's thread and read by `accept_loop()` on the accept thread. Beyond
the visibility issue, `stop()` closed the descriptor and set `-1` while the
accept thread could still be between its `running_` check and its `accept()`
call — so a descriptor number reused by any other thread in the process could
have been accepted on. The unsynchronised `close()` was also the only thing
that woke a blocking `accept()`.

*Fix:* `std::atomic<int>`; `start()` publishes the descriptor only once fully
listening; `accept_loop()` polls with a 100 ms timeout so `running_` is the
single race-free shutdown signal; `stop()` joins the accept thread **before**
closing, removing the reuse window entirely.

## Directory traversal (Phase 8 task 10)

**HLS: not applicable by construction.** `hls::SegmentStore` keeps segments in
memory and looks them up by exact map key; nothing in the HLS path touches the
filesystem. `control/hls_http_handler.cpp` additionally rejects `.`, `..` and
backslash path components as defence in depth.

**Recording: the obligation was unowned.** `recording::AsyncFileSink::open()`
took an already-formed path, and nothing in the tree built that path from the
peer-supplied application and stream names — so there was no sanitisation code
to review, and any future caller would have had to reinvent it.

`recording/recording_path.hpp` now owns it, using an **allow-list, not a
deny-list**. A component is accepted only if every byte is in `[A-Za-z0-9._-]`,
it is not `.` or `..`, it does not begin with `-` (argv injection into operator
tooling) or `.` (hidden from glob-based retention sweeps), and it is at most
100 bytes. Traversal, absolute paths, NUL truncation and separator injection
are therefore *unrepresentable* rather than filtered — which is the point:
deny-lists for path traversal have a long history of bypasses
(`....//`, `..%2f`, overlong UTF-8, `..\`).

Unsafe names are **rejected**, not silently rewritten: a recording stored
under a name different from the one requested breaks the mapping the
management API and the retention sweeper depend on.

`tests/recording/recording_path_test.cpp` is an adversarial corpus covering
`../../../etc/passwd`, bare aliases, absolute-path injection, forward and back
slashes, embedded NUL, percent-encoded and overlong-UTF-8 traversal, shell and
glob metacharacters, over-long components, and a property-style check that no
accepted path escapes the configured root.

## Input size and recursion limits (Phase 8 tasks 5 and 6)

Audit of every client-controlled length that reaches an allocation:

| Limit | Enforced | Where |
|---|---|---|
| AMF0 nesting depth | **Added Phase 8** | `amf0::kMaxNestingDepth` = 32 |
| AMF0 strict-array count | **Added Phase 8** | validated against bytes remaining |
| AMF0 string / long-string length | Pre-existing | `require()` bounds-checks before reading |
| AMF0 property-list iterations | Pre-existing | 1 000 000 sanity cap |
| RTMP message size | Pre-existing | `max_message_size`, checked before reserve |
| Chunk size (Set Chunk Size) | **Added Phase 8** | capped at spec maximum `0xFFFFFF` |
| Concurrent chunk stream IDs | **Added Phase 8** | `ChunkDecoderLimits::max_chunk_streams` |
| Outstanding reassembly bytes | **Added Phase 8** | `ChunkDecoderLimits::max_buffered_payload_bytes` |
| Playback query length / pair count / value length | **Added Phase 8** | `management::kMaxQueryLength` etc. |
| Recording path component length | **Added Phase 8** | `recording::kMaxComponentLength` |
| HTTP header / body bytes | Pre-existing | `HttpServerOptions` |
| Per-viewer queue bytes/packets | Pre-existing | `ServerConfig`, enforced by `ViewerQueue` |
| GOP cache bytes/packets/duration | Pre-existing | `ServerConfig`, enforced by `GopCache` |
| Recording queue bytes | Pre-existing | `RecorderConfig::max_queued_bytes` |
| HLS segment store bytes/count | Pre-existing | `SegmentStoreConfig` |
| Connections per IP, viewers per stream | Pre-existing | `AuthenticatorLimits` |

The playback query parser was also unbounded before Phase 8: it built one
`unordered_map` entry per `&`-separated pair from a string limited only by the
RTMP message size, so a single `play` command could force ~2.5 million map
insertions pre-authentication. It is now a bounded, allocation-light scan for
exactly the two fields the token scheme defines
(`management::parse_playback_query`).

## Integer overflow review (Phase 8 task 8)

Reviewed every arithmetic expression combining a client-controlled value with
a size or offset in the chunk decoder, AMF0 decoder, FLV parser, HTTP server
and token/query parser.

- **`parse_flv` `DataOffset` bound** — the one real finding; see above. Fixed,
  and written as a subtraction specifically so the check itself cannot wrap.
- `amf0::require(data, offset, needed)` — `offset` is bounded by `data.size()`
  and `needed` by a 32-bit field, so on a 64-bit `size_t` the sum cannot wrap.
  Not portable to a 32-bit target; recorded in `docs/production-readiness.md`.
- `ChunkDecoder` message length is a 24-bit field checked against
  `max_message_size` *before* it is used, and `payload_slice` is a `min()`, so
  neither can exceed the buffer.
- FLV `next = pos + kTagHeaderSize + tag.data_size` — `pos <= size` and
  `data_size` is 24-bit, so no wrap on `size_t`.
- HTTP `Content-Length` is parsed into `size_t` and checked against
  `max_body_bytes` before any allocation.

## Timestamp rollover review (Phase 8 task 9)

RTMP timestamps are unsigned 32-bit milliseconds and are **specified to wrap**
(~49.7 days of continuous stream). The decoder computes `base + delta` in
`std::uint32_t`, which wraps by definition — this is correct, not a bug, and
must not be "fixed" into saturation.

Reviewed and found correct; no change required. Locked down by
`ChunkSecurityLimitsTest.TimestampWrapsModulo32BitsRatherThanSaturating`,
which drives a base of `0xFFFFFFF0` through the extended-timestamp path plus a
`0x20` delta and asserts the result is `0x10`.

## Rate limiting (Phase 8 task 7)

`authentication::RtmpAuthenticator` enforces per-IP concurrent connections,
per-stream viewer counts, and an authentication-failure lockout (N failures
per IP within a rolling window → fail closed). The management API has its own
bad-token lockout (`ManagementApiTest.RepeatedBadTokensLockOutTheIp`).

**Open gap:** these are *concurrency* and *failure* limits, not connection
*rate* limits, and all of them are keyed on client IP — which behind a
TLS-terminating proxy is the proxy's address, because the server does not
parse the PROXY protocol. See `docs/tls.md` "PROXY protocol — required, not
optional" and `docs/production-readiness.md`.

## Fuzzing (Phase 8 task 4)

All four parsers the spec names now have harnesses, and all of them were
**actually run**, not merely wired up:

| Target | Executions (ASan+UBSan, 2 seeds) | Result |
|---|---|---|
| `fuzz_amf0_decoder` | 2 500 000 | clean |
| `fuzz_chunk_decoder` | 2 500 000 | clean |
| `fuzz_flv_parser` | 2 500 000 | **found finding 4**; clean after fix |
| `fuzz_token_parser` | 2 500 000 | clean |
| `fuzz_handshake` | 400 000 | clean |

`fuzz_handshake` and `fuzz_token_parser` are new in Phase 8. Apple Clang does
not ship libFuzzer, so `fuzz/fuzz_main.hpp` provides a seeded deterministic
mutation driver that runs with no engine — weaker than libFuzzer (no coverage
feedback) but genuinely executed and fully reproducible from its seed. The
libFuzzer build (`cmake --preset fuzz`) remains the preferred path on Linux CI.

Every harness carries a **structure-aware seed corpus**, which matters more
than the mutator: random bytes are rejected by the first marker/version/
signature check, so without valid seeds a fuzzer only ever exercises the first
error path.

## Sanitizers and static analysis (Phase 8 task 11)

| Tool | Status |
|---|---|
| ASan + UBSan | **Run**, 469/469 tests clean (`asan-core-only`) |
| UBSan alone | Preset added (`ubsan`, `ubsan-core-only`) |
| TSan | **Run**, 469/469 clean — after fixing the race it found (finding 5) |
| Compiler warnings | Zero first-party warnings from a clean build, at `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wsign-conversion -Wformat=2 -Wundef -Wnull-dereference -Wdouble-promotion` |
| clang-tidy | **Not available on this host** (not installed; no Homebrew LLVM). Wired via `-DRTMP_SERVER_ENABLE_CLANG_TIDY=ON` for CI. |
| cppcheck | **Not available on this host.** |
| Clang static analyzer | **Run** (`clang --analyze`) over all Phase 8 changed files. Two findings, both false positives: `unix.BlockInCriticalSection` on `recv()` in `read_request`, where the analyzer does not model the inner scope in `worker_loop()` releasing `queue_mutex_` before `handle_connection()` is called. Reviewed and dismissed with that evidence. |

TSan was previously reported as not run on this host. That was worth
revisiting: it builds and runs fine against the `core-only` subset, and doing
so immediately found a real bug. It covers the genuinely concurrent core
components (`AsyncFileSink` writer thread, `LiveFanout`, `SegmentStore`,
`StreamRegistry`, `RtmpAuthenticator`, `HttpServer`); the io_uring worker pool
still needs a Linux host.

## TLS

Reverse-proxy termination, not native RTMPS. The decision, the reasoning on
both sides, the required protocol/cipher configuration, and the operational
constraints are in `docs/tls.md`.
