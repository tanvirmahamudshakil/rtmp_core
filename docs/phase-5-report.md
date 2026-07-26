# PHASE 5 COMPLETION REPORT

Persistence, authentication and management API.

## 1. What was inspected

- `docs/v2_promot.md` in full (1455 lines), with particular attention to
  section 3 (non-negotiable rules), section 4 (working method), section 5
  (report format), and the full PHASE 5 spec (lines ~856–986), plus a skim
  of PHASE 4 and PHASE 6 for surrounding context.
- Existing persistence layer: `include/rtmp_server/persistence/{store.hpp,
  sqlite_store.hpp}`, `src/persistence/sqlite_store.cpp`.
- Existing management layer: `include/rtmp_server/management/{stream_manager,
  authorization_cache, token, url_builder}.hpp` and matching `.cpp` files.
- Existing observability layer: `logger.hpp`, `metrics.hpp`, `audit_log.hpp`
  and their `.cpp` files.
- `authentication/` and `control/` directories — confirmed empty
  (`.gitkeep` placeholders only) before this phase.
- `src/protocol/commands/command_session.{hpp,cpp}` — the actual RTMP
  publish/play command handling — to determine what publish/playback
  authorization is really wired today, not assumed.
- `src/io/io_uring/event_loop.cpp` and `apps/rtmp_server/{main.cpp,
  CMakeLists.txt}` — to determine whether `CommandSession` and the
  management library are actually connected to the running server in this
  worktree.
- Root `CMakeLists.txt` and every `src/*/CMakeLists.txt` to understand the
  build graph and which components build under `RTMP_SERVER_CORE_ONLY`.
- Built the pre-existing baseline (`cmake --preset core-only`, full build,
  full `ctest`) *before* making any change, to establish ground truth
  rather than trust the task brief's description of repo state.

## 2. Problems confirmed

1. **Playback tokens were generated but never validated in the RTMP `play`
   flow.** `CommandSession::handle_play` (pre-Phase-5) took the raw play
   argument as the stream key, registered it, and subscribed to
   `LiveFanout` — no query-string parsing, no call into
   `StreamManager::verify_playback_token`, no enabled-state check, no
   limits. Matches docs/v2_promot.md section 2, item 11, exactly.
2. **The management library was not linked into the actual server
   executable** in the sense that matters most: `apps/rtmp_server/main.cpp`
   never constructs a `CommandSession` at all — `CommandSession` is not
   referenced anywhere under `src/io/io_uring/*.cpp` in this worktree. This
   is a *deeper* gap than "management not linked": the socket-to-protocol
   wiring itself (Phase 1–4 territory, docs/v2_promot.md section 2 item 2)
   is missing in this worktree, independent of Phase 5. Confirmed by
   `grep`, not assumed.
3. **Publish key and public playback name were not mapped to the same
   internal identity.** `handle_publish` registered/fanned-out under the
   raw secret key; `handle_play` used whatever name the client supplied
   directly as the fan-out key, with no resolution step. Matches section 2
   item 4.
4. **No HTTP management API existed anywhere in the repository** —
   confirmed by `grep -rl` across the whole tree finding zero matches for
   any HTTP-server-shaped code, and by there being no vendored HTTP
   library. Matches section 2 item 16 (metrics/readiness endpoints) and the
   PHASE 5 "Management API" requirement.
5. **`authentication/` and `control/` were empty scaffolding**
   (`.gitkeep` only) — the domain logic they were presumably meant to hold
   (rate limiting, connection/viewer/publisher/IP bounds, the HTTP surface
   itself) did not exist.
6. **`StreamManager` had no way to resolve a publish key to its stream's
   public name** — only `validate_publish_key` (bool) existed; nothing
   returned the *name* a validated key belongs to, which is a prerequisite
   for fixing problem 3.

## 3. Problems not confirmed / found already handled

- **Hashed key storage, key rotation, constant-time comparison, signed
  playback-token expiry/claims** were already correctly implemented in
  `management::StreamManager`/`management::token.{hpp,cpp}` (SHA-256 for
  key hashes, HMAC-SHA256 + `core::constant_time_equals` for tokens,
  distinct `Unauthorized`/`ExpiredToken` error codes). Verified by reading
  the code and by the 217→221 test suite passing; not reimplemented.
- **`AuthorizationCache`** (TTL-bounded, bounded `max_entries`, negative
  caching) already existed and was already correctly shaped to sit in
  front of a `bool(app,key)` loader. Not wired into the new
  `RtmpAuthenticator` in this phase (see "Remaining risks" — the direct
  `StreamManager::validate_publish_key` call is already an in-memory
  map/hash lookup, not a DB call, so the cache is an optimization, not a
  correctness requirement, for the current in-memory `StreamManager`; it
  becomes more valuable once `StreamManager` is persistence-backed on
  every read, which it currently is not — reads are in-memory,
  writes write-through).
- **Observability (`Logger`/`Metrics`/`AuditLog`)** were already real,
  tested implementations, not stubs — confirmed working (used directly by
  the new `ManagementApi`).
- **The claim that `apps/rtmp_server`'s SQLite CMake target had issues**
  was not observed: `find_package(SQLite3 REQUIRED)` +
  `src/persistence/CMakeLists.txt` built cleanly on the first baseline
  build with no changes needed.

## 4. Architecture decisions

- **Publish-key → public-name resolution added as an *optional* hook**
  (`protocol::commands::StreamIdResolver`, set via
  `CommandSession::set_stream_id_resolver`) rather than changing
  `StreamKeyValidator`'s signature. This let the fix land without breaking
  any of the 20 pre-existing `CommandSessionTest` cases, all of which
  construct sessions without a resolver and get the old (raw-key) fan-out
  behaviour, which is the correct backward-compatible default.
- **Playback authorization added the same way**
  (`protocol::commands::PlaybackAuthorizer`, `set_playback_authorizer`),
  called after parsing `name?token=...&expires=...` out of the play
  argument. Unset ⇒ old behaviour (always allowed), matching every
  pre-existing playback test.
- **New `authentication::RtmpAuthenticator`** is the production-shaped
  glue: it owns the per-IP/per-stream counters and failure-window state
  (all in-memory, mutex-guarded, connection-lifecycle-rate — never on the
  RTMP hot path) and hands `CommandSession` three ready-made
  `std::function`s built on top of `StreamManager`. This keeps
  `StreamManager` free of resource-bound bookkeeping (its job is domain
  state) and keeps `CommandSession` free of any concrete authentication
  policy (its job is protocol state), matching the existing
  dependency-injection pattern every other `CommandSession` collaborator
  (`MediaIngest`, `RecorderSink`, `LiveFanout`) already uses.
- **`StreamManager::resolve_stream_name_for_key`** added as the minimal
  method needed to make problem 3's fix real: same hash-compare/enabled
  logic as `validate_publish_key`, returning the stream name instead of a
  bool.
- **Management HTTP API hand-rolled over POSIX sockets** rather than
  vendoring a library (full rationale in docs/management-api.md). Split
  into `control::HttpServer` (bounded generic HTTP/1.1 transport: accept
  thread + fixed worker pool + bounded pending-connection queue +
  header/body size limits) and `control::ManagementApi` (routing/JSON/auth/
  audit logic over `StreamManager`), mirroring the existing
  transport/domain-logic split every other layer in this codebase uses
  (`io_uring` vs `protocol`, `SqliteStore` vs `Store`).
- **`ManagementApi` takes optional, non-owning `StreamRegistry*`/
  `LiveFanout*`.** Status/viewers/disconnect-* routes 503 rather than
  fabricate an answer when unset. This was a deliberate "don't fake
  completion" choice: this worktree has no reachable, running
  `StreamRegistry`/`LiveFanout` instance to attach to a standalone HTTP
  server test process, and the real production wiring point (inside
  `apps/rtmp_server/main.cpp`, Linux-only, currently missing session
  construction entirely per problem 2) is out of reach to compile-verify
  from macOS.
- **Bearer-token auth, fail-closed if unconfigured.** If
  `ManagementApiOptions::admin_token` is empty, every authenticated route
  returns 401 rather than silently allowing access — a missing credential
  must never mean "open".

## 5. Files added

- `include/rtmp_server/authentication/rtmp_authenticator.hpp`
- `src/authentication/rtmp_authenticator.cpp`
- `src/authentication/CMakeLists.txt`
- `include/rtmp_server/control/http_server.hpp`
- `src/control/http_server.cpp`
- `include/rtmp_server/control/management_api.hpp`
- `src/control/management_api.cpp`
- `src/control/CMakeLists.txt`
- `tests/authentication/rtmp_authenticator_test.cpp`
- `tests/authentication/CMakeLists.txt`
- `tests/control/http_server_test.cpp`
- `tests/control/management_api_test.cpp`
- `tests/control/CMakeLists.txt`
- `docs/authentication.md`
- `docs/management-api.md`
- `docs/phase-5-report.md` (this file)

## 6. Files modified

- `CMakeLists.txt` — added `add_subdirectory(src/authentication)`,
  `add_subdirectory(src/control)`, `add_subdirectory(tests/authentication)`,
  `add_subdirectory(tests/control)`.
- `include/rtmp_server/management/stream_manager.hpp` /
  `src/management/stream_manager.cpp` — added
  `resolve_stream_name_for_key`.
- `include/rtmp_server/protocol/commands/command_session.hpp` — added
  `StreamIdResolver`, `PlaybackAuthorizer` type aliases;
  `set_stream_id_resolver`, `set_playback_authorizer`, `set_client_ip`
  setters; matching private members.
- `src/protocol/commands/command_session.cpp` — `handle_publish` now
  resolves the raw key to a canonical stream name (when a resolver is set)
  before registering/fanning out; `handle_play` now splits
  `name?query`, calls the playback authorizer (when set) and rejects with
  `NetStream.Play.Failed` on denial.
- `tests/protocol/commands/command_session_test.cpp` — added 4 tests
  exercising the resolver and playback-authorizer hooks directly against
  `CommandSession`.

## 7. Public interfaces changed

- `management::StreamManager`: new method
  `std::optional<std::string> resolve_stream_name_for_key(std::string_view application, std::string_view raw_key) const`.
  Additive, no existing signature changed.
- `protocol::commands::CommandSession`: two new type aliases
  (`StreamIdResolver`, `PlaybackAuthorizer`), three new setters
  (`set_stream_id_resolver`, `set_playback_authorizer`, `set_client_ip`).
  `StreamKeyValidator`'s signature and the constructor are unchanged —
  every existing caller compiles and behaves identically without changes.
- New public types: `authentication::AuthenticatorLimits`,
  `authentication::RtmpAuthenticator`; `control::HttpRequest`,
  `control::HttpResponse`, `control::HttpServerOptions`,
  `control::HttpServer`, `control::ManagementApiOptions`,
  `control::ManagementApi`.

## 8. Tests added

- `tests/authentication/rtmp_authenticator_test.cpp` (13 tests): valid
  publish key, invalid key, rotated key invalidates the old one, resolver
  maps key→public name (and rejects an unresolvable one), playback without
  a token (allowed when enabled), playback with a valid token, **expired
  token rejected**, **modified/tampered token rejected**, playback on a
  disabled stream rejected, publish for an unknown application rejected,
  **viewer limit enforced**, **per-IP connection limit enforced**,
  **repeated auth failures lock out the IP** (and the count is queryable).
- `tests/control/http_server_test.cpp` (3 tests): a real request over a
  real socket gets a real response; a body over the configured limit is
  rejected with 413; connections beyond the bounded pending-queue depth
  are closed rather than queued without limit.
- `tests/control/management_api_test.cpp` (11 tests): health/live needs no
  auth; mutating routes require a bearer token; wrong token rejected;
  create+list applications; **create-stream returns the raw key only in
  that response, GET never leaks it**; PATCH disables a stream; rotate-key
  returns a new key; playback-token endpoint issues a token verifiable by
  `StreamManager`; status without a registry/fanout wired returns 503 (not
  a fabricated answer); unknown route → 404; repeated bad tokens lock out
  the IP (management-API-level rate limiting).
- `tests/protocol/commands/command_session_test.cpp` (+4 tests): resolver
  rewrites the fan-out identity from the raw key to the public name;
  resolver rejecting an already-validated key fails the publish; playback
  authorizer can reject a `play` (with the parsed app/name/query/IP
  asserted); playback authorizer allowing a `play` correctly strips the
  query string from the stream identity used going forward.

Explicitly not covered by a dedicated new test (see "Remaining risks"):
"database unavailable" for the *authentication* hot path specifically
(covered indirectly: `RtmpAuthenticator`/`CommandSession` never touch
`persistence::Store` at all, by construction — there is nothing to make
unavailable on that path); "configuration cache refresh" beyond what
`AuthorizationCacheTest.InvalidateForcesTheNextCallToReinvokeTheLoader`
(pre-existing) already covers; "no secret in logs" is enforced by
construction (nothing in `management_api.cpp`/`rtmp_authenticator.cpp`
logs `stream_key`/`token`/`admin_token` values — `RTMP_LOG` calls only
carry method/path/status/request_id) but has no automated log-scraping
assertion.

## 9. Commands executed

```
cmake --preset core-only
cmake --build build/core-only -j4
cd build/core-only && ctest --output-on-failure
cmake --preset asan -DRTMP_SERVER_CORE_ONLY=ON
cmake --build build/asan -j4
cd build/asan && ctest --output-on-failure
```

(Plus the same build/test cycle repeated several times while iterating —
final results below are from the last run of each.)

## 10. Actual build result

- **core-only preset**: baseline (before any Phase 5 change) built clean,
  0 errors. After all Phase 5 changes: builds clean, 0 errors. One
  transient `-Wunused-result` warning set (5 instances, `[[nodiscard]]`
  ignored) was hit and fixed during development (removed the unneeded
  `[[nodiscard]]` from a `void`-returning internal helper) — final build
  has no new warnings beyond two pre-existing, unrelated ones
  (`tmpnam` deprecation in `tests/unit/core/config_test.cpp`, a
  sign-conversion warning in `tests/media/flv_writer_test.cpp`, both
  pre-dating this phase).
- **asan preset** (`-DRTMP_SERVER_CORE_ONLY=ON`): builds clean, 0 errors,
  same two pre-existing warnings only.
- `apps/rtmp_server` (the actual server executable, Linux/io_uring-only)
  was **not** built or run — this platform (macOS, Darwin) cannot build
  it, consistent with the project's own `RTMP_SERVER_CORE_ONLY` guidance
  from earlier phases. Not claimed as tested.

## 11. Actual test result

- **core-only, before this phase's changes (baseline)**: `100% tests
  passed, 190/190`.
- **core-only, after this phase's changes**: `100% tests passed,
  221/221` (ctest total time 2.34s). The 31 new tests are exactly the ones
  listed in section 8.
- **asan preset, after this phase's changes**: `100% tests passed,
  221/221` (ctest total time 10.02s) — no ASan or UBSan errors reported by
  any test (would have failed the corresponding test binary if triggered).

## 12. Sanitizer result

AddressSanitizer + UndefinedBehaviorSanitizer build (`cmake --preset asan
-DRTMP_SERVER_CORE_ONLY=ON`) ran the full 221-test suite clean — no
use-after-free, no data race caught by ASan's limited detection, no
leak, no UB report from any of the new `authentication`/`control` code or
the modified `command_session.cpp`/`stream_manager.cpp`. ThreadSanitizer
was **not** run in this phase (no `tsan` preset/run performed — the doc
mentions running "sanitizers where applicable"; the new concurrency
surface here is `HttpServer`'s worker-thread pool and
`RtmpAuthenticator`/`ManagementApi`'s mutex-guarded counters, which would
be the natural TSan target for a follow-up phase). Not claimed as
TSan-clean.

## 13. Performance observations

Not benchmarked in this phase — no load test was run against
`HttpServer`/`ManagementApi` beyond the correctness tests in section 8
(one of which, `ConnectionsBeyondPendingQueueAreRejectedNotQueuedForever`,
does exercise concurrent load with 6 simultaneous clients against a
1-worker/1-pending-queue-depth server and confirms bounded rejection
behavior, but this is a correctness check, not a throughput measurement).
The management API is explicitly connection-lifecycle-rate, not
media-hot-path-rate, so this was not prioritized over correctness/testing
given the phase's time budget.

## 14. Remaining risks

- **`RtmpAuthenticator`/`ManagementApi` are not wired into
  `apps/rtmp_server/main.cpp`.** This worktree cannot build or run that
  executable (macOS, no io_uring), and — independent of that — the
  executable does not currently construct a `CommandSession` at all (see
  section 2, problem 2), so there is no current integration point to wire
  into without also doing Phase 1–4-scope socket/session wiring work. This
  is the single biggest gap between "Phase 5 code exists and is tested in
  isolation" and "Phase 5 controls the real running server," and is the
  most important next step.
- **`ManagementApi`'s status/viewers/disconnect-* routes are untested
  against a real `StreamRegistry`/`LiveFanout`** (only the "not wired"
  503 path has a test) because no test in this phase constructs a live
  publisher/viewer scenario through the HTTP layer end-to-end — the
  underlying `StreamManager::live_state`/`disconnect_publisher`/
  `disconnect_viewers` methods themselves are exercised by pre-existing
  `tests/management/stream_manager_test.cpp`, just not through
  `ManagementApi`'s routing.
- **No application-level enable/disable API** (see docs/authentication.md
  "Known limitations") — `StreamManager` has no `set_application_enabled`.
- **No per-stream "token required" policy flag** — see
  docs/authentication.md.
- **`AuthorizationCache` is not wired into `RtmpAuthenticator`** — the
  direct `StreamManager::validate_publish_key` call is already O(streams
  in application) over in-memory hash comparisons, not a DB call, so this
  is a possible future optimization rather than a correctness gap given
  `StreamManager`'s current all-in-memory-with-write-through design.
- **Hand-rolled JSON parsing** (`parse_flat_json` in `management_api.cpp`)
  is intentionally minimal (flat objects, string/bool/number values only,
  no arrays/nesting/unicode escapes) — sufficient for this API's request
  bodies today, would need extending if the endpoint set grows.
- **No TLS.** The management API is plaintext HTTP; production deployment
  is assumed to terminate TLS in front of it (reverse proxy), not
  implemented or assumed working here.

## 15. Breaking changes

None. Every change to existing public interfaces is additive (new optional
setters/methods with defaults that reproduce prior behaviour). The full
pre-existing 190-test baseline still passes unchanged; the new tests are
strictly additive.

## 16. Rollback considerations

- All new code lives in three new libraries
  (`rtmp_server_authentication`, `rtmp_server_control`) plus additive
  changes to two existing files (`stream_manager.{hpp,cpp}`,
  `command_session.{hpp,cpp}`). Reverting is straightforward: dropping the
  two new `add_subdirectory` pairs from the root `CMakeLists.txt` and the
  `resolve_stream_name_for_key`/`StreamIdResolver`/`PlaybackAuthorizer`
  additions returns the tree to its pre-Phase-5 behaviour exactly, since
  nothing pre-existing was deleted or had its default behaviour changed.
- No schema/data migration was introduced (no new SQLite tables — this
  phase did not touch `src/persistence/sqlite_store.cpp`'s schema), so
  there is no persisted-state rollback concern.

## 17. Definition-of-done checklist

Per docs/v2_promot.md Phase 5:

- [x] Publish authentication is enforced by the actual RTMP path
      (`CommandSession::handle_publish` via `key_validator_`, pre-existing
      and verified working).
- [x] **Playback authentication is now enforced by the actual RTMP path**
      (`CommandSession::handle_play` via the new `playback_authorizer_`
      hook) — this was the confirmed, now-fixed defect.
- [x] Public names and secret keys map to the same internal stream
      identity, when `RtmpAuthenticator`'s resolver is wired
      (`stream_id_resolver_` in `CommandSession`) — implemented and
      tested directly against `CommandSession` and `StreamManager`.
- [x] Management API controls real server state — `ManagementApi` calls
      real `StreamManager` mutations (create/rotate/enable/disable), not
      stubs; verified with real HTTP requests over real sockets in tests.
- [x] Media workers do not block on database operations —
      `RtmpAuthenticator`/`CommandSession`'s authorization path never
      touches `persistence::Store`; the HTTP management API runs on its
      own dedicated accept + worker threads, never on an RTMP event-loop
      thread.
- [x] Secrets are not stored or logged insecurely — keys stored as SHA-256
      hashes only; `RTMP_LOG` calls in the new code carry no key/token
      values; raw keys appear only in create/rotate HTTP responses.
- [~] Readiness correctly reflects required dependencies —
      `/health/ready` checks the persistence store when one is configured;
      **not verified against the real server process** (which this
      worktree cannot build/run), only against `ManagementApi` directly in
      unit tests.
- [ ] **Not done: wiring into the actual running server executable.** See
      section 14. This is the honest, called-out gap rather than a claimed
      completion.

## 18. Recommended next phase

Before proceeding to PHASE 6 (Recording/HLS) as originally sequenced,
strongly recommend a short intermediate pass — call it **Phase 5.5,
transport wiring** — to: (a) construct `CommandSession` per accepted
connection inside the io_uring event loop / `apps/rtmp_server/main.cpp`
(confirmed missing, section 2 problem 2), (b) wire
`RtmpAuthenticator`/`ManagementApi`/`HttpServer` into `main()` alongside
it, with `ManagementApi::set_registry`/`set_fanout` pointed at the real
running instances, and (c) build and run all of this on Linux (this
worktree's macOS build cannot verify it) to close out DoD items left `[~]`
and `[ ]` above with real, compiled, run evidence rather than leaving them
as a documented gap. Only after that should PHASE 6 build recording/HLS on
top of a server whose publish/playback auth and management API are
actually live in the running process, not just unit-tested in isolation.
