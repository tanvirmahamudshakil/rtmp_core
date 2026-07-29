# Control API (Management API — Phase 8)

> Deployment note: StreamForge now runs in open-access mode. The production
> API does not expose publish-key rotation or playback-token endpoints, and
> OBS publishes with the public stream name. The key/token primitives
> documented below remain internal library history and are not required by
> operators or viewers.

## Scope

Phase 8 implements the **domain logic** behind the management API described
in `docs/rtmp_promot.md` "Management API" and "RTMP Link Generation":
applications, streams, stream creation, key generation/rotation, signed
playback tokens, enable/disable, disconnect controls, recording controls,
and live-state snapshots. It does **not** stand up an HTTP server — see
"What this phase deliberately does not do" below.

## Where the code lives

- `include/rtmp_server/management/stream_manager.hpp` /
  `src/management/stream_manager.cpp` — `management::StreamManager`, the
  single entry point for everything above: Application/Stream CRUD, key
  generation/rotation, enable/disable, recording flags, disconnect controls,
  live-state snapshots.
- `include/rtmp_server/management/token.hpp` / `src/management/token.cpp` —
  `sign_token`/`verify_token`: stateless, deterministic HMAC-signed
  playback/publish tokens with expiry.
- `include/rtmp_server/management/url_builder.hpp` /
  `src/management/url_builder.cpp` — pure `build_publish_url`/
  `build_playback_url`/`append_signed_token` string builders.
- `include/rtmp_server/core/hmac.hpp` / `src/core/hmac.cpp` — new core
  primitives this phase needed and every future phase can reuse:
  `hmac_sha256_hex`, `sha256_hex`, `constant_time_equals` (all OpenSSL-backed,
  same `OpenSSL::Crypto` link `core/random.hpp` already uses).

## Domain model

```
Application { name, enabled }
Stream      { application, name, enabled, recording_enabled, created_at }
```

A `Stream`'s publish key is **never stored in plaintext** — only
`sha256_hex(raw_key)` is kept (`StreamManager`'s private `StreamRecord::
key_hash`). The raw key is handed back to the caller exactly twice: from
`create_stream()` and from `rotate_key()`, matching docs/rtmp_promot.md
"Stream Creation Response" — "Only return the raw stream key: during
creation, during explicit key rotation. Do not include raw keys in ordinary
list responses" (`find_stream`/`list_streams` return `Stream`, which has no
key field at all).

## Link generation

`StreamManager::create_stream()` returns both URLs at once:

- **Publish URL** — `rtmp://<host>:<port>/<app>/<raw-key>`: the secret path
  segment used by the publisher's RTMP client.
- **Playback URL** — `rtmp://<host>:<port>/<app>/<stream-name>`: the public
  path segment used by viewers. Never contains the secret key
  (`UrlBuilderTest.PlaybackUrlNeverContainsTheStreamKey` is a regression
  guard for exactly this property).

A signed variant is built by appending a token: `management::
append_signed_token(playback_url, StreamManager::sign_playback_token(...),
expires_at_unix)` → `.../<name>?token=<token>&expires=<unix-time>`.

## Signed tokens

`sign_token`/`verify_token` (`token.hpp`) are **stateless**: the token is
`HMAC-SHA256(secret, application + ":" + name + ":" + expires_at_unix)`,
hex-encoded. Verification recomputes the same HMAC and constant-time-compares
it, then checks `now <= expires_at`. No token store, no revocation list, no
database round-trip — the expiry is carried in (and covered by the signature
of) the URL itself.

`secret` is always `ServerConfig::token_signing_secret` (already declared and
validated non-empty/non-default in `core/config.cpp`) — a single
deployment-wide secret, not per-stream, so rotating a stream's *publish* key
never invalidates already-issued *playback* tokens for it (they're
independent credentials for independent operations, matching the phase spec
listing "key rotation" and "token expiry" as separate acceptance criteria).

`StreamManager::verify_playback_token()` additionally checks the stream is
currently `enabled` before delegating to `verify_token()` — a valid,
unexpired signature for a disabled stream is still rejected
(`Unauthorized`), matching "disabled stream is rejected" applying to
playback tokens too, not just publish keys.

## Enable/disable

`Stream::enabled` gates two independent things:

- `StreamManager::validate_publish_key()` — an unknown key and a
  known-but-disabled stream's key both simply return `false` (indistinguishable
  to the caller), per docs/rtmp_promot.md "do not allow key enumeration".
  Wired directly as a `protocol::commands::StreamKeyValidator`:
  ```cpp
  session.set_stream_key_validator( // conceptually — CommandSession takes
      [&manager](std::string_view app, std::string_view key) {
          return manager.validate_publish_key(app, key);
      });
  ```
  (`CommandSession`'s constructor already takes a `StreamKeyValidator`; the
  validator itself was previously always a test/deployment-supplied
  `std::function` — `StreamManager::validate_publish_key` is simply a real
  implementation of that same shape.)
- `StreamManager::verify_playback_token()` — see above.

`Application::enabled` gates the whole application: `validate_publish_key`
returns `false` immediately if the application itself is disabled, without
even checking individual streams.

## Disconnect controls

Neither publisher nor viewer sessions are reachable from the management
layer as live objects (`CommandSession` is transport-independent and owns no
socket; see docs/rtmp-commands.md and docs/rtmp-playback.md). `StreamManager`
therefore resolves *which* connection(s) to disconnect and hands off the
actual disconnection to an injected handler:

- `disconnect_publisher(app, name, registry)` — scans `registry.snapshot()`
  for a live publisher whose stream key hashes to this stream's stored
  `key_hash`, and if found, calls `PublisherDisconnectHandler(connection_id)`.
  Fails `NotFound` if the stream isn't currently being published.
- `disconnect_viewers(app, name, registry)` — same resolution, but calls
  `ViewerDisconnectHandler(stream_key)` with the raw key, since viewer
  sessions aren't tracked by `connection_id` anywhere yet (`LiveFanout`'s
  subscriber IDs are relay pointers, not connection IDs — see
  docs/rtmp-playback.md "Subscriber IDs are relay addresses"). The handler's
  job — enumerate every `CommandSession` currently playing that key and close
  it — has no owner yet, same standing gap as the rest of the transport
  wiring (see Known limitations).

Both handlers are optional (`std::function`, default empty) — with no
handler set, resolution still succeeds/fails correctly and is fully testable
(see `StreamManagerTest.PublisherCanBeDisconnectedByApi`/
`ViewerSessionsCanBeDisconnectedByApi`), just without an actual socket being
closed, matching this phase's scope (domain logic, not transport).

## Recording controls

`Stream::recording_enabled`, settable via `set_recording_enabled()`, is
metadata only in this phase — nothing yet reads it to decide whether to
attach a `recording::Recorder` to a publishing `CommandSession` (that wiring
belongs together with the rest of the transport-layer wiring, see Known
limitations). It exists now so the management API surface and its tests are
complete; the flag is exactly what a future wiring step would branch on.

## Live-state endpoints

`StreamManager::live_state(registry, fanout)` returns one `LiveState{
application, name, is_live, viewer_count }` per known stream: `is_live` comes
from the same registry-scan-by-key-hash `disconnect_publisher` uses,
`viewer_count` from `LiveFanout::subscriber_count()` on the resolved raw key
(0 if not currently live — `LiveFanout` never associates a viewer count with
a stream key nobody is publishing under).

## API security

- **Constant-time comparisons everywhere a secret is checked**:
  `core::constant_time_equals` (OpenSSL `CRYPTO_memcmp`) is used for token
  signature verification and stream-key hash matching — never `==` on a
  `std::string` holding secret-derived material.
- **Hashed key persistence**: see Domain model above — raw keys are never
  retained past the call that generated them.
- **No key enumeration**: `validate_publish_key` collapses "unknown key" and
  "disabled stream" into the same `false`; nothing in this module ever
  returns "the key was correct but..." for an invalid credential.
- **No secrets in logs**: this module does no logging at all — callers that
  do log (a future HTTP layer) must not log raw keys/tokens/secrets, same
  requirement docs/rtmp_promot.md "Authentication" states explicitly.
- **API-authentication secret**: `ServerConfig::api_authentication_secret` is
  already declared/validated (`core/config.cpp`) for gating the management
  API itself (distinct from stream-level tokens); a future HTTP layer should
  compare it with `core::constant_time_equals`, same as everything else here.

## What this phase deliberately does not do

- **No HTTP server.** No HTTP or JSON library is fetched
  (`cmake/Dependencies.cmake` still only fetches googletest); the phase spec
  allows one ("a small, well-maintained JSON library for the management
  API") but adding an HTTP transport is a separate, transport-layer-shaped
  concern — exactly the same category of thing every phase since Phase 4 has
  deferred for the *RTMP* side (no io_uring transport wiring on this macOS
  host either). `StreamManager` is the part of an HTTP-facing management API
  that's actually testable without a socket, and is built and tested the
  same way `StreamRegistry`/`LiveFanout` were: pure logic, `core-only`
  buildable, real unit tests instead of integration tests against a live
  server.
- **No persistence.** Applications/streams live only in `StreamManager`'s
  in-memory maps and are lost on restart — Phase 9 ("Persistence and
  Production Hardening") is explicitly where SQLite/PostgreSQL-backed
  storage belongs per the phase list.
- **No wiring of `validate_publish_key` into a running `CommandSession`,
  no wiring of `recording_enabled` into `Recorder` attachment, no wiring of
  the disconnect handlers into a real connection-closing mechanism.** All
  three require the same not-yet-built transport/session-owner layer every
  prior phase has deferred (see docs/phase7-checklist.md "Known
  limitations", docs/phase6-checklist.md, etc.) — Phase 8 provides the pieces
  that layer will call into, not the layer itself.

See `docs/phase8-checklist.md` for the full acceptance-criteria-by-test
mapping and further known limitations.
