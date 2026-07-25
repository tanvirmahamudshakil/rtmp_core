# Phase 9 Implementation Checklist — Persistence and Production Hardening

Per user instruction for this phase: implement everything verifiable without
a Docker daemon (none is available on this development host); Docker's own
artifact is still produced (it's cheap and part of the spec), but building/
running it — and the two other host-dependent pieces, systemd and
libFuzzer — are explicitly out of scope for *verification* here. Each is
clearly marked "not verified in this environment" below and in the docs it's
described in, rather than silently claimed as tested.

- [x] SQLite development persistence — `persistence::SqliteStore` (real,
  fully implemented and tested against a real SQLite engine — both
  `:memory:` and on-disk with a close/reopen round-trip)
- [x] PostgreSQL production persistence — **not implemented**, see Known
  limitations. `persistence::Store` is the storage-engine-independent
  interface a `PostgresStore` would implement; none was written (no libpq
  dependency available/testable in this environment, and untested code
  behind an interface is worse than an honestly-documented gap)
- [x] authorization cache — `management::AuthorizationCache`: TTL-bounded,
  caches both positive and negative results, `invalidate()` for
  rotation/enable-disable
- [x] audit logs — `observability::AuditLog`: bounded ring buffer,
  wired into every `StreamManager` mutation (create/delete/rotate/
  enable/disable/disconnect), records outcome not just intent
- [x] metrics — `observability::Metrics`: counter/gauge registry, wired into
  the same `StreamManager` mutations as `management.<action>_total` counters
- [x] Docker — `deploy/docker/Dockerfile` + `.dockerignore` written, matches
  documented build commands; **not built/run** (no Docker daemon on this
  host)
- [x] systemd — `deploy/systemd/rtmp-server.service` written, hardened per
  `systemd.exec(5)`; **not installed/run** (no systemd on this host)
- [x] fuzzing — three libFuzzer-shaped harnesses (`fuzz/fuzz_amf0_decoder`,
  `fuzz_chunk_decoder`, `fuzz_flv_parser`); built and smoke-tested as
  standalone corpus-replay tools; **not run under actual libFuzzer** (Apple
  Clang lacks the libFuzzer runtime — see Known limitations)
- [x] load testing — `apps/load_bench`: in-process synthetic load generator
  over the real `CommandSession`/`StreamRegistry`/`LiveFanout` classes;
  built and run (2.4M fanned-out messages/sec, 4 streams × 50 viewers × 1000
  frames, on this host)
- [x] static analysis — `.clang-tidy`/`cmake/StaticAnalysis.cmake` (already
  existed since Phase 0, applies to all new headers via its
  `HeaderFilterRegex`); **not run** (`clang-tidy` not installed on this host)
- [x] production documentation — `docs/deployment.md`, `docs/security.md`,
  `docs/testing.md` filled in from Phase 0 stubs
- [x] tests — 34 new GoogleTest cases (7 `SqliteStoreTest` + 5
  `AuthorizationCacheTest` + 5 `StreamManagerPersistenceTest` + 9 `HmacTest`
  + 3 `AuditLogTest` + 5 `MetricsTest`), see below

## Files created

- `include/rtmp_server/persistence/store.hpp` (abstract `Store` interface,
  `ApplicationRow`/`StreamRow`)
- `include/rtmp_server/persistence/sqlite_store.hpp`,
  `src/persistence/sqlite_store.cpp` (`SqliteStore`)
- `src/persistence/CMakeLists.txt` (`rtmp_server_persistence`,
  `find_package(SQLite3 REQUIRED)`)
- `include/rtmp_server/management/authorization_cache.hpp`,
  `src/management/authorization_cache.cpp` (`AuthorizationCache`)
- `include/rtmp_server/observability/audit_log.hpp`,
  `src/observability/audit_log.cpp` (`AuditLog`, `AuditEntry`)
- `include/rtmp_server/observability/metrics.hpp`,
  `src/observability/metrics.cpp` (`Metrics`)
- `apps/load_bench/main.cpp`, `apps/load_bench/CMakeLists.txt`
- `fuzz/fuzz_amf0_decoder.cpp`, `fuzz/fuzz_chunk_decoder.cpp`,
  `fuzz/fuzz_flv_parser.cpp`, `fuzz/CMakeLists.txt`
  (`RTMP_SERVER_ENABLE_FUZZING` option)
- `deploy/docker/Dockerfile`, `.dockerignore`
- `deploy/systemd/rtmp-server.service`
- `tests/persistence/sqlite_store_test.cpp`,
  `tests/persistence/CMakeLists.txt`
- `tests/management/authorization_cache_test.cpp`,
  `tests/management/stream_manager_persistence_test.cpp`
- `tests/unit/core/hmac_test.cpp`
- `tests/unit/observability/audit_log_test.cpp`,
  `tests/unit/observability/metrics_test.cpp`
- `docs/phase9-checklist.md` (this file)

## Files changed

- `include/rtmp_server/management/stream_manager.hpp` /
  `src/management/stream_manager.cpp` — added `set_store`/`load_from_store`,
  `set_audit_log`, `set_metrics`, and write-through/audit/metrics calls in
  every mutating method
- `src/management/CMakeLists.txt` — added `authorization_cache.cpp`, linked
  `rtmp_server_persistence`
- `src/core/CMakeLists.txt` — added `observability/audit_log.cpp`,
  `observability/metrics.cpp` (same target `logger.cpp` already lives in)
- `CMakeLists.txt` (root) — added `src/persistence`, `apps/load_bench`,
  `fuzz` (gated on `RTMP_SERVER_BUILD_TESTS`), `tests/persistence`
  subdirectories
- `tests/unit/CMakeLists.txt`, `tests/management/CMakeLists.txt` — added new
  test sources / linked `rtmp_server_persistence`
- `docs/deployment.md`, `docs/security.md`, `docs/testing.md` — filled in
  from Phase 0 stubs

## Architecture decisions

- **`persistence::Store` is a narrow, storage-engine-independent interface**
  (four row types' worth of CRUD, nothing ORM-like), so `StreamManager`
  depends on an abstraction, not on SQLite specifically — same
  `RecorderSink`/`PlaybackSink` pattern used in Phases 6-7 for the same
  reason (swap the concrete implementation without touching the caller).
  `PostgresStore` is a real, expected future implementation of this same
  interface, not a hypothetical.
- **No PostgreSQL implementation, documented rather than stubbed.** Writing
  a `PostgresStore` that links against libpq but has never actually run
  against a real Postgres in this environment would be worse than not
  having one — untested code behind a plausible-looking interface invites
  false confidence. SQLite is genuinely production-viable for the
  single-node deployment model this phase targets (it's not a toy/dev-only
  engine), so this isn't a functionality gap for that deployment shape, only
  for horizontally-scaled multi-node deployments.
- **Store writes are best-effort, never roll back the in-memory operation.**
  Same "storage failure must not take down the hot path" posture Phase 6's
  `recording::Recorder` established for disk errors: a `StreamManager`
  mutation that partially fails (in-memory succeeds, store write fails) is
  recorded as a failed audit entry but the in-memory state — which is what
  every other component (`validate_publish_key`, live-state, disconnect
  resolution) actually reads — stays correct and available. A future
  reconciliation pass (Phase 9+ hardening, not implemented) could compare
  in-memory state against the store and retry failed writes.
- **Authorization cache negative-caches on purpose.** Caching "this key is
  invalid" protects against exactly the workload that matters most for this
  cache: a client retrying a bad/rotated key repeatedly, or an attacker
  probing keys. See `docs/security.md` "Authentication and authorization".
- **Audit log and metrics are separate, both optional, both non-owning
  pointers into `StreamManager`** — same injection pattern as `store_`
  itself and every other collaborator since Phase 5 (`MediaIngest`,
  `RecorderSink`, `LiveFanout`). Kept as two distinct types rather than
  merged because they answer different questions (audit: "what happened and
  did it succeed", metrics: "how often, aggregated") and have different
  retention shapes (bounded ring buffer vs. monotonic counters).
- **Fuzz harnesses double as standalone corpus-replay tools.** Each
  `fuzz_*.cpp` compiles either with libFuzzer's `main()` (when
  `RTMP_SERVER_ENABLE_FUZZING=ON`) or with its own trivial `main()` that
  reads files from argv (default) — so the exact same `LLVMFuzzerTestOneInput`
  function is reachable and testable even on a host without libFuzzer, which
  turned out to be necessary (see Known limitations) rather than
  theoretical.
- **`load_bench` measures the protocol/fan-out layer, not I/O**, by design —
  it's the layer this codebase actually owns and can exercise meaningfully
  without the (not-yet-built-on-this-host) transport; see
  `docs/testing.md` "Load testing" for the full rationale.

## Build commands

```
$ cmake --preset core-only
$ cmake --build --preset core-only
```

Clean Release build (separately verified for the "clean release build"
acceptance criterion, using a scratch build directory not committed to the
repo):

```
$ cmake -S . -B build/release -DRTMP_SERVER_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Release -DRTMP_SERVER_BUILD_TESTS=ON -G Ninja
$ cmake --build build/release -j8
$ ctest --test-dir build/release
```

## Test commands

```
$ ctest --preset core-only
```

## Actual test results

```
100% tests passed out of 190
Total Test time (real) =   1.67 sec
```

190 total: 156 pre-existing (Phase 0-8) + 34 new Phase 9 tests
(`SqliteStoreTest` ×7, `AuthorizationCacheTest` ×5,
`StreamManagerPersistenceTest` ×5, `HmacTest` ×9, `AuditLogTest` ×3,
`MetricsTest` ×5). `HmacTest` covers primitives added back in Phase 8
(`core/hmac.hpp`) that had no dedicated unit test until now — see
"Sanitizer results" below.

Also verified clean under a from-scratch Release build (see Build commands
above): 190/190 passed there too.

## Sanitizer results

```
$ cmake --preset asan
$ cmake --build --preset asan
$ ctest --preset asan
100% tests passed out of 190
Total Test time (real) =   8.82 sec
```

190/190 clean under ASan+UBSan, including every new SQLite C-API call
(`Statement` RAII wrapper, bind/step/column paths) and the OpenSSL
digest/HMAC calls added in `core/hmac.cpp` back in Phase 8 (tested for the
first time this phase — `core/hmac.hpp` had no dedicated unit test until
`hmac_test.cpp`, only indirect coverage via `token_test.cpp`; adding direct
tests with known SHA-256 test vectors was part of this phase's "all tests
pass"/hardening scope).

## Acceptance criteria evidence

- **clean release build** — verified via the scratch `build/release`
  directory above: configures and builds with zero errors (one pre-existing,
  unrelated `-Wunused-function` warning in `flv_writer.cpp` predating this
  phase, and one pre-existing `-Wdeprecated-declarations` in
  `config_test.cpp`'s use of `std::tmpnam` — neither introduced by Phase 9).
- **all tests pass** — 190/190, see "Actual test results".
- **sanitizer builds pass** — 190/190 under ASan+UBSan, see "Sanitizer
  results".
- **startup validation works** — pre-existing `core::ServerConfig::validate()`
  (Phase 1), covered by `tests/unit/core/config_test.cpp` (rejects
  `CHANGE_ME`/empty secrets, rejects zero ports, etc.) — reconfirmed still
  passing as part of this phase's full-suite run; no changes needed since it
  already covered exactly what "startup validation works" requires.
- **systemd deployment works** — **not verified**: `deploy/systemd/
  rtmp-server.service` is written and structurally follows systemd
  conventions, but was never installed or started (no systemd on this host).
  See `docs/deployment.md` "systemd".
- **Docker deployment works** — **not verified**: `deploy/docker/Dockerfile`
  is written and follows this project's own documented build commands, but
  was never built or run (no Docker daemon on this host, and the user
  explicitly scoped this phase to skip Docker verification). See
  `docs/deployment.md` "Docker".
- **security review findings are documented** — `docs/security.md`,
  consolidating cryptography/authN/authZ/input-handling/logging/audit/
  process-hardening posture across every phase, plus a dedicated "Known
  limitations" section listing every open finding (no rate limiting, no
  wired-in publish/playback authorization, no PostgreSQL, unverified
  Docker/systemd/fuzzing/clang-tidy).

## Known limitations

- **PostgreSQL persistence is not implemented** (see "Architecture
  decisions"). `persistence::Store` is ready for a `PostgresStore`; none
  exists.
- **Docker and systemd artifacts are unverified** in this environment (no
  Docker daemon, no systemd) — explicitly out of scope for this phase per
  user instruction. Review both before relying on them.
- **libFuzzer-based fuzzing did not run.** Apple Clang (Xcode's bundled
  toolchain, the only Clang on this host) does not ship
  `libclang_rt.fuzzer_osx.a` — attempting `-DRTMP_SERVER_ENABLE_FUZZING=ON`
  fails at the link step (confirmed: `ld: library ... fuzzer_osx.a not
  found`). The harnesses build and run correctly as standalone corpus-replay
  tools (smoke-tested with random/garbage input, no crashes), but have
  accumulated zero actual fuzzing time/coverage. Should work as documented
  on Linux with a full LLVM toolchain, or macOS with Homebrew's `llvm`
  package.
- **`clang-tidy` was not run** — not installed on this host. The
  `.clang-tidy`/`RTMP_SERVER_ENABLE_CLANG_TIDY` wiring has existed since
  Phase 0 and applies to every file added since; a CI environment with
  `clang-tidy` available should run it before merging.
- **`validate_publish_key`/`verify_playback_token`/disconnect handlers are
  still not wired into any running `CommandSession`** — same standing gap
  `docs/control-api.md` (Phase 8) already documented; unchanged by Phase 9
  because it needs the same not-yet-built transport/session-owner layer.
- **`load_bench` measures the protocol/fan-out layer only**, not real
  network or disk I/O — see `docs/testing.md` "Load testing".
- **No reconciliation between in-memory `StreamManager` state and a failed
  store write** — see "Architecture decisions" ("Store writes are
  best-effort").
- **`AuthorizationCache` is not yet wired into `StreamManager`/
  `validate_publish_key`** — it's implemented and tested standalone, but
  `StreamManager::validate_publish_key` still does its own hash lookup
  directly rather than going through a cache in front of it; wiring it in
  is straightforward (wrap the existing method as the cache's `Loader`) but
  wasn't done since nothing yet calls `validate_publish_key` on a path where
  the cache's benefit (avoiding a DB read) would materialize (no DB read
  happens there today — it's an in-memory map lookup already). It becomes
  load-bearing once persistence-backed authorization actually replaces the
  in-memory-only fast path, which hasn't happened.

## Security concerns

See `docs/security.md` in full — key points repeated here per the phase
report template:

- No hand-rolled cryptography; all randomness/hashing/HMAC/constant-time
  comparison goes through OpenSSL via `core/random.hpp`/`core/hmac.hpp`.
- Raw stream keys and signed tokens are never logged, persisted in
  plaintext, or included in audit-log detail strings.
- SQLite parameter binding (`sqlite3_bind_text`/`sqlite3_bind_int*`) is used
  throughout `SqliteStore` — no string-concatenated SQL, no injection
  surface from application/stream names.
- Audit log and metrics are bounded (ring buffer, no unbounded map growth)
  — same "never let one component grow memory without limit" posture every
  prior phase's bounded-queue components established.

## Performance concerns

- `SqliteStore` operations are synchronous/blocking (`sqlite3_step` on the
  calling thread) — acceptable because they only ever run at management-API
  call rates (create/rotate/enable/disable), never per-packet; this matches
  docs/rtmp_promot.md "Persistence"'s explicit warning against blocking
  database queries "for every media packet" — there are none here.
- `AuthorizationCache::authorize` is O(1) amortized (hash map lookup +
  optional loader call), and its eviction-on-overflow is O(1) (erases
  `begin()`, not a real LRU scan) — deliberately cheap over precise, per its
  own doc comment.
- `AuditLog`/`Metrics` operations are O(1) map/deque operations under a
  single mutex each — fine at management-API-call rates, not designed for
  (and not used on) the per-packet media path.
- `load_bench` itself demonstrates the fan-out path's throughput ceiling on
  this host: ~2.4M delivered viewer messages/sec across 4 streams × 50
  viewers, with zero network/disk I/O in the loop (by design — see "Known
  limitations").

## Next phase

Phase 9 is the last phase listed in docs/rtmp_promot.md. Remaining
follow-on work (not a numbered phase): wire persistence-backed
authorization + `AuthorizationCache` + disconnect handlers into a real
transport/session-owner layer once one exists on a Linux host; implement
`PostgresStore`; verify Docker/systemd/libFuzzer/clang-tidy on a Linux CI
host; stand up the HTTP layer for the management API (`docs/control-api.md`
"What this phase deliberately does not do").
