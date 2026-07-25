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

## Known limitations (security review findings)

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
