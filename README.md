# rtmp

Production-grade raw C++23 RTMP streaming server for Linux, built on `io_uring`.

> Status: Phase 0 complete (repository inspection, design, scaffolding). No
> executable code yet — see `docs/architecture.md` and `docs/phase0-checklist.md`.

## Project Purpose

Accepts RTMP publishing (OBS/encoders), authenticates publishers via stream
keys and signed tokens, fans a single incoming H.264/AAC stream out to
multiple RTMP viewers, records to FLV, and exposes a secure HTTP management
API for applications/streams/tokens/recordings. Pass-through only: no
encoding, decoding, or transcoding in this phase.

## Architecture Summary

Layered: Management API -> Stream Registry -> Publisher/Subscriber sessions
-> RTMP Protocol Engine (handshake/chunk/AMF0/commands) -> Async Transport
Layer (`io_uring` only) -> FLV Recorder / Metrics-Logs. Full detail in
`docs/architecture.md`.

## Supported Features (target, by phase)

See `docs/architecture.md` section 13 for the phase-by-phase build order
(Phase 1 core+io_uring TCP through Phase 9 persistence+hardening).

## Unsupported Features

Media encoding/decoding/transcoding (H.264, H.265, AV1, AAC), HLS, DASH, SRT,
WebRTC, RTMPS, clustering, CDN, GPU acceleration — all designed as future
extension points (`docs/future-roadmap.md`), not implemented here.

## Dependencies

C++23 stdlib, POSIX sockets, `liburing`, CMake, Ninja, Clang/GCC, OpenSSL,
SQLite (dev) / PostgreSQL (prod), GoogleTest or Catch2, a small JSON library.
No FFmpeg/libav*, no Boost.Asio, no existing RTMP server source. Full list in
`docs/rtmp_promot.md`.

## Ubuntu Installation Steps

TODO: filled in once Phase 1 lands (CMake/Ninja/liburing package setup).

## Build Steps

TODO: filled in once Phase 1 lands (`scripts/build-debug.sh`, `scripts/build-release.sh`).

## Run Steps

```bash
./rtmp_server --config ./config/server.yaml
```

(target invocation; binary not yet implemented)

## Configuration

See `config/server.example.yaml` and `docs/configuration.md`.

## OBS Setup

```text
Service: Custom
Server: rtmp://SERVER_IP:1935/live
Stream Key: GENERATED_STREAM_KEY
```

## Playback Setup

```text
rtmp://SERVER_IP:1935/live/STREAM_NAME
```

## API Examples

See `docs/control-api.md` (stub, filled in during Phase 8).

## Testing

See `docs/testing.md` and `scripts/run-tests.sh` (stub, filled in from Phase 1 onward).

## Sanitizer Commands

See `scripts/run-sanitizers.sh` (stub, filled in from Phase 1 onward).

## Docker Deployment

See `deploy/docker/` and `docs/deployment.md` (stub, filled in during Phase 9).

## systemd Deployment

See `deploy/systemd/` and `docs/deployment.md` (stub, filled in during Phase 9).

## Security Notes

See `docs/security.md`.

## Troubleshooting

See `docs/troubleshooting.md`.

## Future Roadmap

See `docs/future-roadmap.md`.
