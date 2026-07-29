# Management HTTP API (Phase 5)

> Current production mode is intentionally open-access. The server does not
> require a bearer header, and every stream exposes one `rtmp_url` used for
> both publishing and playback. Publish-key rotation/playback-token routes
> are not exposed. Historical Phase 5 authentication details below describe
> optional library capability, not the deployed `apps/rtmp_server` behavior.

## Why hand-rolled instead of a vendored library

No HTTP server library was vendored anywhere in this repository before this
phase (no `vendor/`/`third_party/` directory, no existing `FetchContent`
for one). Two options were considered:

1. `FetchContent`-vendor a header-only library (e.g. cpp-httplib).
2. Hand-roll a minimal bounded server over POSIX sockets.

Option 2 was chosen to match the codebase's existing dependency posture:
every other Phase 5-adjacent component (`src/persistence` → system
SQLite3, `core::hmac` → system OpenSSL) links a *system* library, never a
fetched one. The endpoint surface needed (a dozen small JSON routes, no
HTTP/2, no TLS termination — that belongs in front of this behind a
reverse proxy in production) is small enough that a ~250-line bounded
server is less risk than pulling in and pinning a third-party dependency.
This is a judgement call, not a mandate from the spec — a future phase
could still vendor a library if the endpoint surface grows.

## Components

- `control::HttpServer` (`include/rtmp_server/control/http_server.hpp`,
  `src/control/http_server.cpp`) — accept thread + fixed worker-thread pool
  over plain POSIX sockets. Bounds enforced (docs/v2_promot.md section
  3.5): `max_pending_requests` (queue between accept and workers — once
  full, new connections are closed, not queued unboundedly),
  `max_header_bytes` (431 if exceeded), `max_body_bytes` (413 if exceeded),
  `listen_backlog`. Runs on its own threads, never on an RTMP/io_uring
  event-loop thread (section 3.6).
- `control::ManagementApi` (`include/rtmp_server/control/management_api.hpp`,
  `src/control/management_api.cpp`) — routes requests onto
  `management::StreamManager`, with open access by default, structured JSON
  errors, `X-Request-Id`, audit logging (via
  `observability::AuditLog`, when injected), and pagination
  (`?limit=&offset=`) on list endpoints.

## Endpoints implemented

| Method | Path                                      | Notes |
|---|---|---|
| GET  | `/health/live`                              | No auth. Always 200 once the process is up. |
| GET  | `/health/ready`                             | No auth. 503 if a `persistence::Store` is configured and unreachable. |
| GET  | `/metrics`                                  | Open. Prometheus-text-ish `name value` lines from `observability::Metrics`. |
| POST | `/v1/applications`                          | `{"name":"..."}` |
| GET  | `/v1/applications`                          | Paginated. |
| POST | `/v1/streams`                               | `{"application":"...","name":"...","recording_enabled":bool}`. Response includes one universal `rtmp_url` for input and output. |
| GET  | `/v1/streams?application=...`               | Paginated by `application` query param (required). |
| GET  | `/v1/streams/{application}:{name}`          | Includes the universal `rtmp_url`; never includes a secret key. |
| PATCH| `/v1/streams/{application}:{name}`          | `{"enabled":bool}` and/or `{"recording_enabled":bool}`. |
| DELETE| `/v1/streams/{application}:{name}`         | Permanently removes the stream and disconnects its active publisher/viewers. |
| GET  | `/v1/streams/{application}:{name}/status`   | Requires a `StreamRegistry`/`LiveFanout` to be wired via `set_registry`/`set_fanout`; 503 otherwise (see Known limitations). |
| GET  | `/v1/streams/{application}:{name}/viewers`  | Same payload as `status` (viewer_count is part of it). |
| POST | `/v1/streams/{application}:{name}/disconnect-publisher` | Requires a `StreamRegistry`; 503 otherwise. |
| POST | `/v1/streams/{application}:{name}/disconnect-viewers`   | Requires a `StreamRegistry`; 503 otherwise. |

Stream identifiers in the path are `<application>:<name>` (colon, not
slash) so they fit in one path segment without percent-encoding.

## Known limitations (not faked, explicitly deferred)

- **Not wired into `apps/rtmp_server/main.cpp`.** `apps/rtmp_server` only
  builds on Linux (io_uring), and this worktree cannot compile or run it on
  macOS — verified in this phase, not assumed. `ManagementApi`/`HttpServer`
  are built, tested, and exercised as a standalone library on this
  platform (`RTMP_SERVER_CORE_ONLY`); wiring them into `main()` alongside
  the io_uring event loop, and connecting `set_registry`/`set_fanout` to
  the real running `StreamRegistry`/`LiveFanout`, is Linux-build work this
  phase could not compile-verify and is left as the next concrete task
  (see docs/phase-5-report.md "Remaining risks").
- **No HTTPS/TLS termination.** Expected to run behind a reverse proxy in
  production, same as most minimal internal admin APIs; not implemented
  here.
- **No JSON schema validation library** — field presence/type is checked by
  hand per endpoint (see `parse_flat_json` in `management_api.cpp`), not a
  general schema validator. Sufficient for the flat request bodies this API
  accepts.
