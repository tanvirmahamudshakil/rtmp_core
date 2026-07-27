# PHASE 8 COMPLETION REPORT

Security hardening, deployment and production release
(`docs/v2_promot.md` PHASE 8, lines 1236-1334).

Baseline at the start of this phase: 407/407 tests passing.
Final: **469/469 passing** in each of three configurations (core-only,
ASan+UBSan, TSan).

---

## 1. What was inspected

- `docs/v2_promot.md` sections 1-5 (rules, working method, report format),
  section 6 (final acceptance criteria), section 7, and PHASE 8 in full;
  PHASE 7 skimmed for the observability and load-testing groundwork.
- Every parser reachable from untrusted input: `protocol::amf0` decoder,
  `protocol::chunk::ChunkDecoder`, `protocol::handshake::HandshakeSession`,
  `media::flv::parse_flv`, `control::HttpServer`'s request reader,
  `control::hls_http_handler`, and the playback-token query parser inside
  `authentication::RtmpAuthenticator`.
- Every filesystem path built from client-controlled input:
  `src/recording/*`, `src/hls/*`, `control/hls_http_handler.cpp`.
- `core::ServerConfig::validate()`, `CMakePresets.json`,
  `cmake/{CompilerWarnings,Sanitizers,StaticAnalysis}.cmake`.
- Existing `fuzz/` harnesses and their build wiring.
- `authentication::RtmpAuthenticator` rate limiting; `management::token`.
- Signal handling and shutdown in `apps/rtmp_server/main.cpp`,
  `src/io/io_uring/worker_pool.cpp`.
- `deploy/systemd/`, `deploy/logrotate/`, `deploy/docker/`.
- Service composition: `apps/rtmp_server/main.cpp` and its `CMakeLists.txt`,
  `IoUringEventLoop::start_rtmp_session`.
- Tooling availability: `clang-tidy`, `cppcheck`, `systemd-analyze`,
  libFuzzer.

## 2. Problems confirmed

Each was reproduced with an executed proof-of-concept against the pre-fix
code before any change was made.

**2.1 AMF0 unbounded recursion — remote unauthenticated crash (critical).**
`decode_value` and `decode_properties` recursed with no depth bound, so AMF0
nesting mapped directly onto call-stack depth. 800 001 bytes of
`03 00 01 61` repeated terminated the process with SIGSEGV (exit 139).
Reachable by any peer that has completed the handshake — no authentication
required.

**2.2 AMF0 strict-array unchecked allocation (high).** The 32-bit element
count went straight to `vector::reserve`. Five bytes (`0A FF FF FF FF`)
requested storage for 4 294 967 295 `Amf0Value`s (~275 GB of address space).

**2.3 Chunk-stream reassembly amplification (high).** `ChunkDecoder` created
per-csid state on demand (65 600 addressable csids) and reserved the full
declared message length upfront. Measured: **2 840 000 bytes of input → 269
MiB RSS / 792 MiB peak footprint** in one decoder. ~95x resident
amplification, per connection, pre-authentication.

**2.4 `parse_flv` out-of-bounds read (medium).** Found by
`fuzz/fuzz_flv_parser` under ASan (container-overflow in `read_u32_be`).
`DataOffset` was validated as `<= data.size()`, but `PreviousTagSize0` is read
as four bytes starting *at* that offset, so any value in `[size-3, size]`
passed and then read past the end.

**2.5 `HttpServer` accept/stop data race (medium).** Found by TSan.
`listen_fd_` was a plain `int` written by `stop()` and read by
`accept_loop()`. Beyond visibility: `stop()` closed the descriptor and set
`-1` while the accept thread could be between its `running_` check and its
`accept()` call, so a descriptor reused by another thread could be accepted
on. The unsynchronised `close()` was also the only thing waking a blocking
`accept()`.

**2.6 Unbounded playback-query parser (medium).** The parser in
`rtmp_authenticator.cpp` built one `unordered_map` entry per `&`-separated
pair from a string bounded only by the 10 MiB RTMP message limit — ~2.5
million insertions from one pre-authentication `play` command.

**2.7 No recording path sanitisation existed at all.** Not a bug in existing
code — `AsyncFileSink::open()` took an already-formed path and *nothing in the
tree built that path* from the peer-supplied application/stream names. The
obligation was unowned.

**2.8 Invalid systemd syscall filter.** The unit had
`SystemCallFilter=@system-service io_uring`. There is no `io_uring` syscall
group and no syscall named `io_uring`; `@system-service` does not include the
io_uring syscalls. The server would have died at startup on EPERM.

**2.9 Five pre-existing compiler warnings** in a build that claims a strict
warning policy.

## 3. Problems not confirmed

- **Timestamp rollover.** Reviewed as required by task 9 and found
  **correct**. RTMP timestamps are unsigned 32-bit milliseconds specified to
  wrap; the decoder computes `base + delta` in `std::uint32_t`, which wraps
  by definition. No change made — "fixing" this into saturation would be a
  regression. Locked down with a test.
- **HLS directory traversal.** Not applicable: `hls::SegmentStore` is
  in-memory and keyed by exact map lookup; nothing in the HLS path touches
  the filesystem. `hls_http_handler.cpp` already rejects `.`/`..`/backslash
  components as defence in depth.
- **Integer overflow beyond 2.4.** `amf0::require`, `ChunkDecoder` length
  arithmetic, FLV tag advance, and HTTP `Content-Length` were each reviewed
  and found sound on a 64-bit `size_t`. (`amf0::require` would be
  wrap-unsafe on a 32-bit target — noted, not a defect on any supported
  platform.)
- **Graceful shutdown missing.** Already present:
  `apps/rtmp_server/main.cpp` installs async-signal-safe SIGTERM/SIGINT
  handlers calling `WorkerPool::stop()`, and ignores SIGPIPE.
- **The `CommandSession` wiring gap flagged by the Phase 5/6 reports.**
  **Has since been closed** — `IoUringEventLoop::start_rtmp_session`
  (`event_loop.cpp:567`) constructs a full `RtmpConnectionSession`. The
  *authorization* and service-layer wiring, however, was never done; see 14.
- **Two Clang static-analyzer findings** (`unix.BlockInCriticalSection` on
  `recv()` in `read_request`) reviewed and dismissed as false positives: the
  inner scope in `worker_loop()` releases `queue_mutex_` before
  `handle_connection()` is called; the checker does not model the scope exit.

## 4. Architecture decisions

**4.1 TLS: reverse-proxy termination, not native RTMPS.** Full reasoning in
`docs/tls.md`. Summary of why native RTMPS was rejected: it must be inserted
into the io_uring transport, where OpenSSL's readiness-oriented BIO model has
to be married to a completion-based ring; per-connection encryption destroys
the shared-immutable-payload fan-out invariant (rule 3.8), reintroducing a
per-viewer copy and putting encryption on the event-loop thread (rule 3.6);
and none of it could be built, run, sanitized or fuzzed on this host. The
costs of the proxy approach are stated plainly, including the plaintext
proxy→server hop and the per-IP rate-limiting consequence.

**4.2 Path safety: allow-list, not deny-list.** Deny-listing `../` has a long
history of bypasses (`....//`, `..%2f`, overlong UTF-8, `..\\`, NUL
truncation). An allow-list of `[A-Za-z0-9._-]` makes those
*unrepresentable*.

**4.3 Unsafe stream names are rejected, not sanitised.** A recording stored
under a name different from the one requested silently breaks the mapping the
management API and retention sweeper depend on.

**4.4 The query parser was extracted rather than patched in place.** In an
anonymous namespace it could be neither unit-tested nor fuzzed. It is now
`management::parse_playback_query`, with both.

**4.5 A standalone mutation fuzzer was written.** Apple Clang does not ship
libFuzzer, so `-DRTMP_SERVER_ENABLE_FUZZING=ON` cannot link here and the
harnesses were corpus replayers with no corpus — wired up and never run,
which rule 3.1 forbids. `fuzz/fuzz_main.hpp` gives a seeded, deterministic,
engine-free mutation driver. Explicitly weaker than libFuzzer (no coverage
feedback); the libFuzzer build remains preferred on Linux CI.

**4.6 Structure-aware seed corpora.** More important than the mutator: random
bytes are rejected by the first marker/version/signature check, so without
valid seeds a fuzzer only ever exercises the first error path.

**4.7 `Set Chunk Size` bounded by the RTMP spec limit, not by
`max_message_size`.** Coupling them rejected conforming peers — caught by an
existing integration test. The actual slice taken is always
`min(bytes_remaining, chunk_size)`, so it is already bounded.

**4.8 Config validation fails startup rather than warning.** A warning nobody
reads is not a release gate.

**4.9 The io_uring service-composition gap was documented, not "fixed".**
Writing several hundred lines of untestable composition code into a target
that cannot be compiled here, then reporting it done, is precisely what rule
3.1 forbids. See 14.

## 5. Files added

```
docs/tls.md
docs/production-readiness.md
docs/phase-8-report.md
include/rtmp_server/management/query_parser.hpp
include/rtmp_server/recording/recording_path.hpp
src/management/query_parser.cpp
src/recording/recording_path.cpp
fuzz/fuzz_main.hpp
fuzz/fuzz_handshake.cpp
fuzz/fuzz_token_parser.cpp
scripts/release_gate.sh
deploy/systemd/rtmp-server.env.example
deploy/logrotate/rtmp-server
tests/protocol/security_limits_test.cpp
tests/management/query_parser_test.cpp
tests/recording/recording_path_test.cpp
```

## 6. Files modified

```
CMakeLists.txt                                  UBSAN/HARDENING options
CMakePresets.json                               production/ubsan/*-core-only/fuzz presets
cmake/Sanitizers.cmake                          UBSan-alone + rtmp_server_set_hardening
src/protocol/amf0/amf0_decoder.cpp              depth bound, strict-array count validation
include/rtmp_server/protocol/amf0/amf0_decoder.hpp   kMaxNestingDepth
src/protocol/chunk/chunk_decoder.cpp            reassembly bounds and accounting
include/rtmp_server/protocol/chunk/chunk_decoder.hpp ChunkDecoderLimits
src/media/flv/flv_writer.cpp                    DataOffset bound; dead function removed
src/control/http_server.cpp                     accept/stop race fix, poll-based accept
include/rtmp_server/control/http_server.hpp     atomic listen_fd_
src/authentication/rtmp_authenticator.cpp       use bounded query parser
src/core/config.cpp                             release-gate validation
include/rtmp_server/core/config.hpp             kMinSecretLength, kMaxSupportedRtmpMessageSize
include/rtmp_server/core/error.hpp              ErrorCode::InvalidArgument (appended)
include/rtmp_server/loadgen/media_source.hpp    [[maybe_unused]] seed_ with rationale
fuzz/CMakeLists.txt, fuzz_{amf0,chunk,flv}*.cpp shared driver + seed corpora
deploy/systemd/rtmp-server.service              syscall filter fix + Phase 8 directives
docs/deployment.md, docs/security.md            rewritten / extended
src/{management,media}/CMakeLists.txt           new sources
tests/{protocol,management,recording}/CMakeLists.txt  new tests
tests/unit/core/config_test.cpp                 gate tests; tmpnam -> mkstemp
tests/media/flv_writer_test.cpp                 DataOffset regressions
tests/hls/hls_http_test.cpp                     discarded-result warning
```

44 files changed, 4066 insertions, 249 deletions.

## 7. Public interfaces changed

All additive; no existing signature changed in a source-breaking way.

- `ChunkDecoder(std::uint32_t max_message_size, ChunkDecoderLimits limits = {})`
  — new **defaulted** parameter; every existing call site compiles unchanged.
  New accessors `limits()`, `buffered_payload_bytes()`, `chunk_stream_count()`.
- `amf0::kMaxNestingDepth` — new constant. `decode`/`decode_all` signatures
  unchanged.
- `core::ErrorCode::InvalidArgument` — **appended** at the end. No exhaustive
  switch over `ErrorCode` exists in the tree; existing values deliberately not
  renumbered because they appear in logs and metrics.
- `core::kMinSecretLength`, `core::kMaxSupportedRtmpMessageSize` — new.
- `management::parse_playback_query`, `split_stream_name`, `PlaybackQuery`,
  `kMaxQueryLength`, `kMaxFieldValueLength`, `kMaxQueryPairs` — new.
- `recording::build_recording_path`, `sanitize_path_component`,
  `is_safe_path_component`, `is_safe_path_char`, `kMaxComponentLength` — new.
- `HttpServer::listen_fd_` — private, `int` → `std::atomic<int>`.
- CMake: `RTMP_SERVER_ENABLE_UBSAN`, `RTMP_SERVER_ENABLE_HARDENING`.

**Behavioural changes that could affect an existing deployment:**

1. Configuration that previously started now fails if secrets are shorter
   than 32 characters, are placeholders, are zero-entropy, or are identical
   to each other. **Intended** — this is the release gate.
2. Peers using more than 64 concurrent chunk stream IDs, or negotiating a
   chunk size above `0xFFFFFF`, are now disconnected. No conforming client
   does either.
3. AMF0 nesting beyond 32 levels is rejected. No real client exceeds 3.

## 8. Tests added

**+62 (407 → 469).**

- `tests/protocol/security_limits_test.cpp` (+21): AMF0 depth including the
  exact 800 KB payload that used to segfault, depth reset between top-level
  values, strict-array count validation, chunk-stream table and reassembly
  byte bounds, budget release on completion and on Abort, Set Chunk Size
  bounds, and 32-bit timestamp rollover.
- `tests/recording/recording_path_test.cpp` (+16): adversarial traversal
  corpus (`../../../etc/passwd`, bare aliases, absolute-path injection,
  forward/back slashes, embedded NUL, percent-encoded and overlong-UTF-8
  traversal, shell/glob metacharacters, leading dash/dot, over-long
  components) plus a property check that no accepted path escapes the root.
- `tests/management/query_parser_test.cpp` (+14): extraction, unknown-param
  tolerance, first-occurrence-wins, strict numeric expiry, length/pair/value
  bounds, a 2 MiB pathological query, binary bytes, and stream-name splitting.
- `tests/unit/core/config_test.cpp` (+9): every release gate.
- `tests/media/flv_writer_test.cpp` (+2): `DataOffset` regressions for the
  fuzzer-found over-read.

## 9. Commands executed

```sh
cmake --preset core-only -DRTMP_SERVER_CORE_ONLY=ON
cmake --build --preset core-only && ctest --preset core-only
cmake --preset asan-core-only && cmake --build --preset asan-core-only && ctest --preset asan-core-only
cmake --preset tsan-core-only && cmake --build --preset tsan-core-only && ctest --preset tsan-core-only

# Pre-fix proof-of-concept harnesses (AMF0 recursion/allocation, chunk amplification)
clang++ -std=c++23 -I include poc.cpp  ... && ./poc 0 ; ./poc 1
clang++ -std=c++23 -I include poc2.cpp ... && /usr/bin/time -l ./poc2

# Fuzzing, ASan+UBSan build, two independent seeds per target
./build/asan-core-only/fuzz/<target> --runs N --seed {20250727,99} --max-len 8192

# Static analysis
clang --analyze -Xclang -analyzer-output=text -std=c++23 -I include <changed files>

# Release gate
SKIP_TSAN=1 FUZZ_RUNS=50000 ./scripts/release_gate.sh

# Tool availability
which clang-tidy cppcheck systemd-analyze     # none present
clang++ -fsanitize=fuzzer,address ...         # libclang_rt.fuzzer_osx.a not found
```

## 10. Actual build result

Clean build from an empty build directory, `core-only`:

```
zero first-party compiler warnings
```

at `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wsign-conversion
-Wformat=2 -Wundef -Wnull-dereference -Wdouble-promotion`. Third-party
(`_deps/`, GoogleTest) and `ld` duplicate-library notices excluded.

The five pre-existing first-party warnings were fixed as part of this phase so
the warning gate is genuinely green from scratch rather than only on an
incremental build.

`asan-core-only` and `tsan-core-only` also build clean.

**Not built:** `src/io/io_uring/`, `apps/rtmp_server` — Linux-only, cannot be
compiled on this host.

## 11. Actual test result

| Configuration | Result |
|---|---|
| `core-only` | **469/469 passed**, 9.2 s |
| `asan-core-only` (ASan + UBSan) | **469/469 passed**, 28.0 s |
| `tsan-core-only` (TSan) | **469/469 passed**, 109 s |

Baseline before this phase: 407/407.

## 12. Sanitizer result

**ASan + UBSan — clean.** 469/469. `-fno-sanitize-recover=all`, so any UBSan
finding aborts. Additionally clean across 10.4M fuzz executions.

**TSan — clean, after fixing the real race it found.** 469/469.

TSan had not been run on this host in previous phases. It was worth
revisiting: it builds and runs fine against the `core-only` subset, and the
first run immediately found finding 2.5 — a genuine cross-thread file
descriptor lifetime bug, not a visibility formality. It covers the genuinely
concurrent core components (`AsyncFileSink` writer thread, `LiveFanout`,
`SegmentStore`, `StreamRegistry`, `RtmpAuthenticator`, `HttpServer`). The
io_uring worker pool still needs a Linux host.

**Fuzzing — one crash found (2.4), fixed, then clean.**

| Target | Executions (ASan+UBSan, 2 seeds) | Rate | Result |
|---|---|---|---|
| `fuzz_amf0_decoder` | 2 500 000 | ~147 k/s | clean |
| `fuzz_chunk_decoder` | 2 500 000 | ~94 k/s | clean |
| `fuzz_flv_parser` | 2 500 000 | ~217 k/s | **found 2.4**; clean after fix |
| `fuzz_token_parser` | 2 500 000 | ~110 k/s | clean |
| `fuzz_handshake` | 400 000 | ~28 k/s | clean |

`fuzz_handshake` is slower because each input is re-fed as randomised
fragments to exercise the state machine across transport-level fragmentation.

**Static analysis — Clang analyzer run; clang-tidy and cppcheck unavailable.**
Neither is installed on this host and no Homebrew LLVM is present
(`brew --prefix llvm` reports a path that does not exist). `clang --analyze`
produced two findings, both reviewed and dismissed as false positives (see 3).

## 13. Performance observations

- **The chunk reassembly fix is a large memory win under attack and free
  otherwise.** The 2.84 MB adversarial input went from 269 MiB RSS / 792 MiB
  peak to 11 MiB peak. Legitimate traffic is unaffected: the removed upfront
  `reserve(declared_length)` is replaced by a bounded initial reserve plus
  geometric growth, so a real publisher sending a real message pays at most a
  couple of extra reallocations per message.
- **The query parser got cheaper.** It no longer allocates a map or a string
  per parameter; it allocates only the two recognised values.
- **`HttpServer` now polls with a 100 ms timeout instead of blocking in
  `accept()`.** Negligible cost (one `poll()` per 100 ms on an idle server)
  and it bounds shutdown latency.
- **Sanitizer overhead**, for CI budgeting: ASan+UBSan ~3x, TSan ~12x
  wall-clock over the suite.
- No throughput or capacity measurement was taken this phase — the load
  generator targets the transport, which cannot run here. The Phase 7
  capacity numbers stand, with the caveat recorded in
  `docs/production-readiness.md`.

## 14. Remaining risks

Full treatment in `docs/production-readiness.md`. The three that block
production:

1. **The deployable binary has no authentication, management, recording, HLS
   or persistence.** `apps/rtmp_server/main.cpp` constructs only
   `StreamRegistry`, `StreamIdRegistry` and `WorkerPool`, and its
   `CMakeLists.txt` links only core, protocol and io_uring — the management,
   authentication, control, media and persistence libraries are not linked
   into the executable at all. `start_rtmp_session` never sets
   `key_validator`, `stream_id_resolver` or `playback_authorizer`. **As built,
   the server accepts any publish key and any playback request.** Not fixed
   here because that target cannot be compiled on this host, and shipping
   untestable composition code as "done" is what rule 3.1 forbids. It is a
   focused task for a Linux host.
2. **The io_uring transport has never been executed** — in any phase. Multiple
   workers, `SO_REUSEPORT`, registered buffers, multishot accept/recv, linked
   timeouts and cross-worker routing are implemented and reviewed, never run.
3. **Per-IP rate limiting is defeated by proxy TLS termination.** The server
   does not parse the PROXY protocol, so behind the chosen TLS strategy every
   connection appears to come from the proxy address: the per-IP cap is
   consumed globally and one attacker's failures lock out everyone. Enabling
   `proxy_protocol` against the current server also *breaks ingest*, since
   `HandshakeSession` consumes the header as a malformed C0.

Lesser risks: no token revocation before expiry and no overlap window on
secret rotation; no connection *rate* limit (only a concurrency cap); no
libFuzzer (so far less coverage per execution than a real fuzzer would give);
systemd/logrotate/Docker artefacts reviewed but never executed;
`amf0::require` assumes 64-bit `size_t`.

## 15. Breaking changes

No source-breaking API change. Three behavioural changes, all intended:

1. **Configuration that previously started may now fail** — short,
   placeholder, zero-entropy or duplicated secrets are rejected. This is the
   Phase 8 release gate working. Operators must generate proper secrets
   (`openssl rand -hex 32`) before upgrading. **This is the one change that
   requires action during an upgrade.**
2. Peers using >64 concurrent chunk stream IDs or negotiating a chunk size
   above `0xFFFFFF` are disconnected. No conforming client does either.
3. AMF0 nesting beyond 32 levels is rejected. No real client exceeds 3.

## 16. Rollback considerations

Each Phase 8 commit is independently revertable, and they are ordered so
earlier ones do not depend on later ones.

- Reverting the **config validation** commit restores the old permissive
  behaviour. Only do this if an emergency deployment is blocked by secret
  length — and then treat weak secrets as an open incident.
- Reverting the **protocol bounds** commit reopens three remotely-triggerable
  resource-exhaustion vulnerabilities, two of them pre-authentication. Do not.
- Reverting the **`HttpServer`** commit reopens a cross-thread fd-lifetime
  bug. Do not.
- The **documentation, preset and script** commits have no runtime effect and
  are safe to revert in isolation.
- Reverting the **fuzz** commit loses two harnesses and the ability to run any
  of them on a host without libFuzzer.

The persistence schema is unchanged this phase, so no database rollback is
involved. General procedure: `docs/deployment.md` "Rollback procedure".

## 17. Definition-of-done checklist

From `docs/v2_promot.md` PHASE 8:

| Item | Status |
|---|---|
| Production build is reproducible | **Partially.** `production` preset added; reproducible given a pinned toolchain, but not bit-for-bit (debug info embeds absolute paths — mitigation documented). Cannot be built on this host. |
| systemd deployment is documented and tested | **Documented, not tested.** Unit written and reviewed line by line, and a real defect in it fixed (invalid `SystemCallFilter`). No systemd on this host; `systemd-analyze verify` unavailable. Live testing requires a Linux host. |
| Graceful shutdown works | **Implemented and documented; not executed.** Handlers present and async-signal-safe; `TimeoutStopSec` sized against recording finalisation. Requires a Linux host to verify. |
| Security limits are enforced | **Met.** Enforced, not merely documented — 62 new tests, and three previously-unbounded limits added after reproducing the exhaustion each allowed. |
| Fuzz tests exist for exposed parsers | **Met.** All four parsers the spec names, plus FLV. Actually run: 10.4M executions under ASan+UBSan, one real bug found. |
| No known critical sanitizer issues remain | **Met.** ASan+UBSan and TSan both 469/469 clean; the one race TSan found is fixed. |
| Rollback and upgrade procedures are documented | **Met.** `docs/deployment.md`. |
| Final architecture documentation matches the code | **Met**, including where it does not flatter the code: `docs/production-readiness.md` states plainly that the deployable binary is missing everything above the protocol layer. |

Security tasks 1-11 and deployment tasks: all addressed; per-task detail in
`docs/security.md` and `docs/deployment.md`.

## 18. Recommended next phase

Phase 8 is the final phase of `docs/v2_promot.md` ("Stop after Phase 8"). No
further phase is defined. What a Linux host must do before this is deployable,
in order:

1. **Close the service-composition gap** (risk 1). Link the management,
   authentication, control, media, hls and persistence libraries into
   `apps/rtmp_server`; construct `SqliteStore`, `StreamManager`,
   `RtmpAuthenticator`, `HttpServer` and `ManagementApi` in `main()`; thread
   `key_validator`, `stream_id_resolver` and `playback_authorizer` through
   `WorkerPool` into `IoUringEventLoop::start_rtmp_session`. Until this is
   done the server is unauthenticated.
2. **Build and test the io_uring transport on Linux** (risk 2): full suite,
   ASan/UBSan, TSan, and the libFuzzer build of all five harnesses.
3. **Re-run the Phase 7 load matrix against `apps/rtmp_server`**, not
   `apps/rtmp_test_server`, and reissue `docs/capacity-report.md` with numbers
   that describe the production transport.
4. **Add PROXY protocol parsing** (risk 3), or enforce equivalent limits at
   the proxy and accept that the server's per-IP limits are inert.
5. **Run `systemd-analyze verify` and `systemd-analyze security`**, then work
   through the pre-flight checklist in `docs/deployment.md`.
6. **Run `clang-tidy` and `cppcheck`** in CI, where they are available.

## Related

- `docs/production-readiness.md` — acceptance criteria cross-check
- `docs/security.md` — security posture and all findings
- `docs/deployment.md` — deployment, kernel tuning, operational procedures
- `docs/tls.md` — TLS strategy decision
