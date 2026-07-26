# Authentication (Phase 5)

## Components

- `management::StreamManager` — domain model (Application/Stream), hashed
  publish keys (`core::sha256_hex`), key rotation, signed playback tokens
  (`management::sign_token`/`verify_token`, HMAC-SHA256, constant-time
  compare), enabled-state checks. Pre-existing (labelled "Phase 8/9" in its
  own comments from earlier work on this repository), verified in this
  phase and extended with `resolve_stream_name_for_key`.
- `authentication::RtmpAuthenticator` (new) — builds the three callbacks
  `protocol::commands::CommandSession` needs (`StreamKeyValidator`,
  `StreamIdResolver`, `PlaybackAuthorizer`) on top of `StreamManager`, and
  enforces the resource bounds required by docs/v2_promot.md section 3.5:
  per-stream viewer count, per-IP connection count, and an authentication
  failure-rate lockout per IP.
- `protocol::commands::CommandSession` (extended) — `handle_publish` now
  resolves the raw key to a canonical public stream name via
  `StreamIdResolver` before registering/fanning out (when a resolver is
  wired); `handle_play` now parses `name?token=...&expires=...`, calls the
  injected `PlaybackAuthorizer`, and rejects with
  `NetStream.Play.Failed` on denial. Both hooks are optional and default to
  pre-Phase-5 behaviour when unset, so every pre-existing test is
  unaffected.

## Publish flow

1. `key_validator_(app, raw_key)` — `RtmpAuthenticator::key_validator()` →
   `StreamManager::validate_publish_key` (SHA-256 hash compare, constant
   time via `core::constant_time_equals`, application/stream enabled
   checked in the same call).
2. On success, `stream_id_resolver_(app, raw_key)` →
   `StreamManager::resolve_stream_name_for_key` returns the stream's public
   `name`. The fan-out registry (`StreamRegistry`) and `LiveFanout` are
   keyed by this public name from this point on, not the raw secret key —
   this is what makes a publish-by-key and a play-by-name converge on one
   internal identity (docs/v2_promot.md Phase 5 item 4).

## Playback flow

1. `play "<name>?token=<sig>&expires=<unix>"` is split into `name` and the
   raw query string by `CommandSession::handle_play`.
2. `RtmpAuthenticator::playback_authorizer()`:
   - checks the per-IP auth-failure lockout,
   - checks stream and application enabled state,
   - if a `token` field is present, validates it via
     `StreamManager::verify_playback_token` (constant-time signature
     compare + expiry check),
   - checks the per-stream viewer limit,
   - records the pass/fail outcome against the caller's IP.

## Known limitations (not faked, explicitly deferred)

- **No blanket "token required" mode.** A stream with no signed URL handed
  out is playable with just its public name. Enforcing "token mandatory for
  every stream" would need a per-stream policy flag that doesn't exist yet
  in `management::Stream` — left for a future phase rather than bolted on
  as an implicit convention.
- **No application-level enable/disable API.** `StreamManager` only exposes
  `set_enabled` per *stream*; there is no `set_application_enabled`. The
  authorizer still checks `find_application(...)->enabled` (true for every
  application returned by `create_application`), but nothing in the public
  API can currently flip it to `false` short of deleting the application.
- **IP-restriction token claims are not implemented.** `token.hpp`'s doc
  comment lists "Optional IP restriction" as a claim the token *could*
  carry; the current `sign_token`/`verify_token` pair only covers
  application/name/expiry. Not added in this phase to avoid changing the
  token wire format without a corresponding management-API field to set it.
- **Publisher-count limiting is effectively enforced by
  `StreamRegistry::register_publisher`'s existing "no second publisher for
  the same key" rule**, not a separate counter in `RtmpAuthenticator`
  (`AuthenticatorLimits::max_publishers_per_stream` exists for
  documentation/future multi-ingest use but isn't separately wired).
- **This authenticator is not wired into `apps/rtmp_server/main.cpp` /
  the io_uring transport in this worktree**, because — verified in this
  phase — `CommandSession` is not constructed anywhere in
  `src/io/io_uring/*.cpp` in this worktree at all (no
  `RtmpConnectionSession`, no session-per-connection wiring). That is a
  pre-existing, pre-Phase-5 transport-integration gap (matches
  docs/v2_promot.md section 2 items 1/2), out of this phase's scope to fix,
  and is called out explicitly in docs/phase-5-report.md rather than
  silently left unmentioned.
