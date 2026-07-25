# Phase 8 Implementation Checklist — Management API and Link Generation

- [x] applications — `management::StreamManager::create_application/
  delete_application/list_applications/find_application`, `Application{name,
  enabled}`
- [x] streams — `Stream{application, name, enabled, recording_enabled,
  created_at}`, `find_stream`/`list_streams`; raw publish key never included
  (only `create_stream`/`rotate_key` ever return it)
- [x] stream creation — `create_stream(app, name, recording_enabled)`
  generates a random key, hashes and stores it, returns
  `StreamCreationResult{stream, stream_key, publish_url, playback_url}`
- [x] key generation — `core::generate_secure_token(24)` (existing
  OpenSSL-`RAND_bytes`-backed primitive from Phase 0/1, reused as-is)
- [x] key rotation — `rotate_key(app, name)` replaces the stored hash with a
  freshly generated key's hash; the old key immediately stops validating
- [x] publish/playback URLs — `management::build_publish_url`/
  `build_playback_url` (`url_builder.hpp`), matching
  docs/rtmp_promot.md "RTMP Link Generation" exactly: publish uses the raw
  key as the path segment, playback uses the public stream name
- [x] signed tokens — `management::sign_token`/`verify_token`
  (`token.hpp`/`.cpp`): stateless `HMAC-SHA256(secret, app:name:expires)`,
  hex-encoded, constant-time verified, expiry checked separately from
  signature validity
- [x] enable/disable — `Stream::enabled` (`set_enabled`), consulted by both
  `validate_publish_key` and `verify_playback_token`; `Application::enabled`
  gates every stream underneath it
- [x] disconnect controls — `disconnect_publisher`/`disconnect_viewers`
  resolve the live connection/stream key via `StreamRegistry` and invoke an
  injected `PublisherDisconnectHandler`/`ViewerDisconnectHandler`
- [x] recording controls — `Stream::recording_enabled`
  (`set_recording_enabled`); metadata-only in this phase (see Known
  limitations)
- [x] live-state endpoints — `live_state(registry, fanout)` returns one
  `LiveState{application, name, is_live, viewer_count}` per known stream
- [x] API security — `core::hmac_sha256_hex`/`sha256_hex`/
  `constant_time_equals` (new `core/hmac.hpp`, OpenSSL-backed); hashed key
  storage; no-key-enumeration (`validate_publish_key` collapses unknown vs.
  disabled to the same `false`); see `docs/control-api.md` "API security"
- [x] tests — 24 new GoogleTest cases (4 `UrlBuilderTest` + 7 `TokenTest` +
  13 `StreamManagerTest`), see below

## Files created

- `include/rtmp_server/core/hmac.hpp`, `src/core/hmac.cpp`
- `include/rtmp_server/management/stream_manager.hpp`,
  `src/management/stream_manager.cpp`
- `include/rtmp_server/management/token.hpp`, `src/management/token.cpp`
- `include/rtmp_server/management/url_builder.hpp`,
  `src/management/url_builder.cpp`
- `src/management/CMakeLists.txt` (`rtmp_server_management` library)
- `tests/management/{url_builder_test,token_test,stream_manager_test}.cpp`,
  `tests/management/CMakeLists.txt`
- `docs/phase8-checklist.md` (this file)

## Files changed

- `include/rtmp_server/core/error.hpp` — added `ErrorCode::NotFound`,
  `ErrorCode::Conflict` (generic domain errors CRUD operations need,
  independent of the existing Protocol/Network/Configuration/Authentication/
  Storage-specific codes)
- `src/core/CMakeLists.txt` — added `hmac.cpp`
- `CMakeLists.txt` (root) — added `add_subdirectory(src/management)`,
  `add_subdirectory(tests/management)`
- `docs/control-api.md` — filled in from the Phase 0 stub

## Architecture decisions

- **Domain logic only, no HTTP server.** Same deferral posture as every
  prior phase's not-wired-into-`event_loop.cpp` gap (Phase 4-7): the phase
  spec's allowed-dependency list permits adding an HTTP/JSON library, but
  doing so is a transport-layer decision (framework choice, threading model,
  request routing) orthogonal to the domain logic the acceptance criteria
  actually exercise ("API creates streams", "API returns secure URLs", "key
  rotation works", ...) — all of which are fully testable by calling
  `StreamManager` directly. Full rationale in `docs/control-api.md` "What
  this phase deliberately does not do".
- **Hashed-at-rest keys, plaintext only at the moment of creation/rotation.**
  `StreamRecord` stores `sha256_hex(raw_key)`, never the raw key; this is a
  direct requirement from docs/rtmp_promot.md ("hashed key persistence where
  practical", "do not allow key enumeration") and costs nothing since
  publish-time validation only ever needs to *compare* keys, never *display*
  them again.
- **Stateless signed tokens, no token store.** A token is fully
  self-describing (HMAC over `app:name:expires`) and re-verifiable from just
  the secret — no revocation list, no per-token database row, no cleanup job.
  Trade-off: revoking a single outstanding token before its natural expiry is
  not possible without also invalidating every other token signed with the
  same secret (global secret rotation) — acceptable for this phase since
  "token revocation" is listed under docs/rtmp_promot.md "Authentication" as
  a *feature area* alongside expiry, not as a Phase 8 acceptance criterion
  (only "token expiry works" is required); true single-token revocation would
  need Phase 9's persistence layer to track issued/revoked token IDs.
- **Disconnect controls resolve-then-delegate, they don't own sockets.**
  `StreamManager` has no reachable socket/connection object (same reasoning
  as `RecorderSink`/`PlaybackSink` in Phases 6-7: the protocol/management
  layers stay transport-independent). It does the part only it can do
  (figuring out *which* connection_id/stream_key corresponds to a management
  API's app+name request, by hash-matching against `StreamRegistry`'s live
  snapshot) and hands off the actual close to an injected handler — same
  "resolve here, act there" split `LiveFanout`'s `PublisherDisconnectHandler`-
  shaped callbacks already established in Phase 7.
- **`core/hmac.hpp` lives in `core`, not `management`.** HMAC/SHA-256/
  constant-time-compare are general-purpose cryptographic primitives (same
  category as `core/random.hpp`, already OpenSSL-linked in
  `src/core/CMakeLists.txt`), not management-API-specific — keeping them in
  `core` means any future phase (e.g. a hashed-password check, a different
  signed-URL scheme) reuses them without a dependency on `management`.

## Build commands

```
$ cmake --preset core-only
$ cmake --build --preset core-only
```

## Test commands

```
$ ctest --preset core-only
```

## Actual test results

```
100% tests passed out of 156
Total Test time (real) =   1.35 sec
```

156 total: 132 pre-existing (Phase 0-7) + 24 new Phase 8 tests
(`UrlBuilderTest` ×4, `TokenTest` ×7, `StreamManagerTest` ×13).

## Sanitizer results

```
$ cmake --preset asan
$ cmake --build --preset asan
$ ctest --preset asan
100% tests passed out of 156
Total Test time (real) =   7.42 sec
```

156/156 clean under ASan+UBSan, including the OpenSSL EVP digest/HMAC calls
and every hash/constant-time-compare path.

## Acceptance criteria evidence

- **API creates streams** — `StreamManagerTest.ApiCreatesStreamsWithSecureUrls`
  and `StreamManagerTest.CreateStreamFailsWithoutAnApplication`/
  `CreateApplicationSucceedsOnceAndRejectsDuplicate` (creation preconditions
  and conflict handling).
- **API returns secure URLs** —
  `StreamManagerTest.ApiCreatesStreamsWithSecureUrls` asserts the exact
  `publish_url`/`playback_url` shapes; `UrlBuilderTest.
  PlaybackUrlNeverContainsTheStreamKey` is a dedicated regression guard that
  the public playback URL never leaks the secret publish key.
- **key rotation works** —
  `StreamManagerTest.KeyRotationInvalidatesTheOldKeyAndActivatesTheNewOne`:
  the pre-rotation key stops validating, the post-rotation key validates.
- **token expiry works** — `TokenTest.ExpiredTokenIsRejectedWithExpiredTokenCode`,
  `TokenTest.TokenAtExactExpiryInstantIsStillValid` (boundary),
  `TokenTest.TamperedExpiryInvalidatesSignature` (can't extend a token by
  editing the query string), and
  `StreamManagerTest.TokenExpiryIsEnforcedThroughTheManager` (same behavior
  through the full `StreamManager` API, not just the standalone `token.hpp`
  functions).
- **disabled stream is rejected** —
  `StreamManagerTest.DisabledStreamRejectsPublishKeyEvenThoughItIsCorrect`
  (publish path) and `StreamManagerTest.PlaybackTokenForADisabledStreamIsRejected`
  (playback-token path — a validly-signed, unexpired token is still rejected
  once the stream is disabled).
- **publisher can be disconnected by API** —
  `StreamManagerTest.PublisherCanBeDisconnectedByApi` (handler receives the
  correct `connection_id`) and `DisconnectPublisherFailsWhenStreamIsNotCurrentlyLive`
  (correctly reports `NotFound` rather than silently no-op'ing when there's
  nothing to disconnect).
- **viewer sessions can be disconnected** —
  `StreamManagerTest.ViewerSessionsCanBeDisconnectedByApi` (handler receives
  the correct raw stream key to hand off to a session-owner sweep).

## Known limitations

- No HTTP server/transport — see `docs/control-api.md` "What this phase
  deliberately does not do" for the full rationale. `StreamManager`/`token.hpp`/
  `url_builder.hpp` are the part of a management API that's transport-
  independent; routing, request parsing, and response serialization (JSON)
  are not implemented.
- No persistence — everything is in-memory and lost on restart; explicitly
  Phase 9's scope ("SQLite development persistence", "PostgreSQL production
  persistence").
- `validate_publish_key` is not wired into any running `CommandSession`'s
  `StreamKeyValidator`, `recording_enabled` is not wired into `Recorder`
  attachment, and the disconnect handlers are not wired into a real
  connection-closing mechanism — all three need the same not-yet-built
  transport/session-owner layer every phase since Phase 4 has deferred.
- Single-token revocation before natural expiry is not possible (stateless
  tokens — see "Architecture decisions"); only whole-secret rotation
  invalidates all outstanding tokens at once.
- No IP restrictions, no single-use tokens, no failed-authentication
  throttling — all listed as optional/available features under
  docs/rtmp_promot.md "Authentication", none required by this phase's
  acceptance criteria.
- `disconnect_publisher`/`disconnect_viewers`/`live_state`'s live-key
  resolution is O(live publisher count) per call (`StreamRegistry::
  snapshot()` + a linear scan); fine at management-API call rates, would
  need an index (e.g. registry keyed by key hash) if ever called on a hot
  path, which it isn't.
- `Application`/`Stream` names are not validated against a character set/
  length limit — a future HTTP layer building URLs and paths from
  user-supplied names should validate/escape them; `StreamManager` itself
  treats names as opaque strings.

## Security concerns

- All secret comparisons (`validate_publish_key`'s key-hash match, token
  signature verification) use `core::constant_time_equals`
  (`CRYPTO_memcmp`), not `std::string::operator==`, so comparison time
  cannot be used to guess a correct prefix.
- No raw key or token is ever logged or persisted by this module (it does no
  logging at all); the only place a raw key exists is the return value of
  `create_stream`/`rotate_key` and the caller's own handling of it.
- `validate_publish_key` deliberately returns the same `false` for "no such
  key", "stream disabled", and "application disabled" — an attacker probing
  keys cannot distinguish "wrong key" from "right key, disabled stream" from
  the return value alone.
- HMAC and SHA-256 are both OpenSSL EVP-backed (`EVP_MD_CTX`/`HMAC()`), the
  same cryptographic library already trusted for `core::secure_random_bytes`
  — no hand-rolled cryptography.

## Performance concerns

- `StreamManager` is entirely in-memory `std::map`/`std::mutex`-guarded
  lookups — no I/O, no blocking, matching the phase spec's later warning (for
  Phase 9) against blocking database queries on any hot path; this phase has
  no database yet at all.
- Token verification is O(1): one HMAC computation plus one constant-time
  compare, no lookup.
- `create_stream`/`rotate_key` each do one `RAND_bytes`-backed 24-byte token
  generation plus one SHA-256 hash — negligible relative to typical
  management-API call rates (human/automation-driven, not per-packet).

## Next phase

Next: Phase 9 — Persistence and Production Hardening (SQLite/PostgreSQL
persistence, authorization cache, audit logs, metrics, Docker, systemd,
fuzzing, load testing, static analysis, production documentation).
