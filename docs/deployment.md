# Deployment

## Scope

How to build, configure, and run `rtmp_server` in production (Phase 9,
docs/rtmp_promot.md "Docker", "systemd"). Covers Docker and systemd; both
artifacts are written to match this project's own documented build/config
conventions but are **not verified in this repository's development
environment** — see "Known limitations" in `docs/phase9-checklist.md`. There
is no Docker daemon and no systemd on the macOS host this codebase was
developed on; the transport layer itself (`apps/rtmp_server`, `src/io/
io_uring`) is Linux-only and has never built on this host either (every
phase since Phase 4 has documented this — see e.g.
`docs/phase7-checklist.md` "Known limitations").

## Building for production

```
$ cmake --preset debug -DCMAKE_BUILD_TYPE=Release -DRTMP_SERVER_BUILD_TESTS=OFF
$ cmake --build build/debug -j"$(nproc)"
```

(The preset is named `debug` for historical reasons — `CMAKE_BUILD_TYPE=
Release` overrides its optimization level; see `CMakePresets.json`.) Requires
a Linux host with liburing, OpenSSL, and SQLite3 development packages
installed (`liburing-dev`, `libssl-dev`, `libsqlite3-dev` on
Debian/Ubuntu) — see `deploy/docker/Dockerfile`'s builder stage for the
exact package list this was validated against structurally (not executed —
see above).

## Configuration

Copy `config/server.example.yaml` and edit it — every key is required (see
`core::ServerConfig::validate()` in `src/core/config.cpp`, the authoritative
source of what's required). At minimum, **before starting**:

- `token_signing_secret` and `api_authentication_secret` must be changed from
  `CHANGE_ME` (or left empty) — `validate()` refuses to start otherwise
  (startup validation; see `docs/phase9-checklist.md` "Acceptance criteria
  evidence" — "startup validation works").
- `public_rtmp_hostname` should be your real public hostname — it's baked
  into every publish/playback URL `management::StreamManager::create_stream`
  returns (see `docs/control-api.md`).
- `database_type`/`database_connection` select the persistence backend (see
  "Persistence" below).

Every key can be overridden by environment variable
`RTMP_SERVER_<UPPER_SNAKE_KEY>` (e.g. `RTMP_SERVER_TOKEN_SIGNING_SECRET=...`)
— see `src/core/config.cpp`'s `apply_env_overrides`. This is the recommended
way to inject secrets in Docker/systemd rather than committing them to
`server.yaml`.

## Persistence

`database_type: sqlite` (the default, `persistence::SqliteStore`) is a
single `.db` file — fully implemented and tested (`tests/persistence/
sqlite_store_test.cpp`), suitable for a single-node deployment. Point
`database_connection` at a path under a persistent volume (Docker) or
`/var/lib/rtmp_server/data` (systemd unit's `ReadWritePaths`).

`database_type: postgresql` is **not implemented** in this phase — see
`docs/phase9-checklist.md` "Known limitations". Multi-node/production-scale
deployments needing PostgreSQL are not yet supported; `SqliteStore` is
usable in production for a single-node deployment (SQLite itself is
production-grade for this workload — the gap is a `PostgresStore`
implementation, not SQLite's suitability).

## Docker

`deploy/docker/Dockerfile` — a two-stage build (Ubuntu 24.04 builder + slim
runtime image), runs as an unprivileged `rtmp_server` user, exposes 1935
(RTMP) and 8080 (management API — not yet an HTTP server, see
`docs/control-api.md`), and bakes in the example config (override via a
bind-mounted `server.yaml` or `RTMP_SERVER_*` env vars).

```
$ docker build -f deploy/docker/Dockerfile -t rtmp_server .
$ docker run -p 1935:1935 -p 8080:8080 \
    -e RTMP_SERVER_TOKEN_SIGNING_SECRET=... \
    -e RTMP_SERVER_API_AUTHENTICATION_SECRET=... \
    -v rtmp_server_data:/var/lib/rtmp_server \
    rtmp_server
```

**Not run in this environment** — see Scope above. Review the Dockerfile
before relying on it in production; it has not been built or smoke-tested.

## systemd

`deploy/systemd/rtmp-server.service` — hardened per `systemd.exec(5)`
(`ProtectSystem=strict`, `NoNewPrivileges`, capability-bounded to
`CAP_NET_BIND_SERVICE`, restrictive `SystemCallFilter`). Install:

```
$ sudo useradd --system --no-create-home --shell /usr/sbin/nologin rtmp_server
$ sudo mkdir -p /etc/rtmp_server /var/lib/rtmp_server
$ sudo cp config/server.example.yaml /etc/rtmp_server/server.yaml   # then edit secrets
$ sudo cp deploy/systemd/rtmp-server.service /etc/systemd/system/
$ sudo chown -R rtmp_server:rtmp_server /var/lib/rtmp_server
$ sudo systemctl daemon-reload
$ sudo systemctl enable --now rtmp-server
```

**Not run in this environment** — same caveat as Docker above.

## Load testing

`apps/load_bench` (Phase 9 "load testing") is an in-process synthetic load
generator for the protocol/fan-out layer — see `docs/testing.md` "Load
testing" for what it measures and why it's shaped this way (no real socket
transport on this host).

```
$ ./build/<preset>/apps/load_bench/load_bench --streams 4 --viewers-per-stream 50 --frames 1000
```

## Monitoring

`observability::Metrics` (Phase 9) accumulates counters/gauges in-process;
`observability::AuditLog` records every management-API mutation. Neither is
yet exposed over HTTP (`/metrics`, `/audit`) — see `docs/control-api.md`
"What this phase deliberately does not do" for why (no HTTP server exists
yet at all). Until that exists, these are only reachable by a process that
embeds `StreamManager` directly and calls `.snapshot()`/
`.counters_snapshot()` itself.
