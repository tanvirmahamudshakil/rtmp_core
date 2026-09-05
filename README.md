# StreamForge RTMP Server

A self-hosted C++23 RTMP/HLS origin for Linux, built on `io_uring`, with a
direct web control plane. It accepts OBS/encoder publishing by public stream
name, fans H.264/AAC RTMP media directly, repackages the same ingest into
cacheable MPEG-TS HLS segments without transcoding, persists application and
stream configuration in SQLite, and exports Prometheus metrics.

This deployment is designed for direct origin delivery. It does not require
or configure a CDN.

## One-command Linux installation

Ubuntu 24.04+ or Debian 13+ with systemd:

```bash
git clone https://github.com/tanvirmahamudshakil/rtmp_core.git
cd rtmp_core
sudo bash scripts/install-linux.sh
```

The installer performs a full clean install by default. On every run it stops
and removes the previous StreamForge services, binaries, configuration,
database, stream keys, recordings, backups, web assets and production build
artefacts before rebuilding everything and generating fresh secrets. Missing
items are skipped. Set `RTMP_FRESH_INSTALL=0` only when an intentional
in-place upgrade must preserve the existing data and credentials.

With no domain, the installer detects the VPS primary IPv4 address and serves
the panel over HTTP on that IP. Add `RTMP_DOMAIN=stream.example.com` when DNS
is ready to let Caddy provision HTTPS automatically.

The installer builds the hardened production binary and admin panel, installs
Caddy with automatic HTTPS, creates an isolated system user, generates
secrets, configures systemd and conservative kernel networking baselines,
detects CPU, RAM, virtualization, the public network interface and link speed,
then runs a readiness check. On a virtual VPS it leaves CPU placement to the
hypervisor-aware scheduler and disables SQPOLL's dedicated polling threads; on
bare metal it enables pinning and SQPOLL. Receive-buffer memory, Varnish cache,
worker count and file-descriptor limits are derived from the detected host.

On dedicated VPS hosts it disables per-request Varnish log consumers. Reading
every segment hit is itself expensive at large audience sizes; aggregate
cache/origin metrics remain available. System journals are capped at 128 MiB
with 1 GiB reserved on the root filesystem.

The installer also enables `streamforge-disk-guard.timer`. Every five minutes
it checks `/var/lib/rtmp-server`'s filesystem. At 80% usage or below 4 GiB
free, it vacuums rebuildable caches and stale recording parts, then—only if
pressure remains—removes the oldest backups/completed recordings while keeping
at least three of each. It never scans the live SQLite database, credentials or
configuration. A successful install removes its production build tree and
`node_modules`; set `RTMP_CLEAN_BUILD_ARTIFACTS=0` only on a development host.

Fresh opens of the production HLS master link use the smaller startup
rendition by default (`kk/KK` starts on `KK_480p`). Existing viewers keep the
rendition URL already loaded. Both playlists and `.ts` segments go through the
local shared Varnish cache, so additional viewers normally produce cache hits
instead of additional origin work. Set the names for another deployment at
install time:

```sh
sudo env \
  RTMP_FAST_JOIN_APPLICATION=sports \
  RTMP_FAST_JOIN_STREAM=match \
  RTMP_FAST_JOIN_RENDITION=match_480p \
  bash scripts/install-linux.sh
```

Use `RTMP_ENABLE_FAST_JOIN=0` when that deployment does not have a dedicated
startup rendition.

Bandwidth detection is automatic. The installer reads the primary NIC's link
speed using Linux sysfs/ethtool; a virtual NIC that hides it gets a non-blocking
20 Gbps planning fallback. Virtual-interface speeds can also be higher than
the bandwidth purchased from the provider, so set the committed Mbps with
`RTMP_BANDWIDTH_MBIT` whenever it differs.

Stream bitrate is automatic by default. The server is a media passthrough: it
does not re-encode or force a bitrate, so viewers receive the bitrate sent by
OBS or an upstream transcoder. After publishing starts, the admin panel first
uses measured egress per active viewer; before a viewer connects, it uses
measured ingress per active publisher. The live estimate refreshes with the
server metrics.

Before the first publisher connects there is no bitrate to measure, but the
installer still has to size finite RAM, socket and file-descriptor resources.
It uses `RTMP_RESOURCE_SIZING_MBIT=3` only for that pre-live capacity estimate;
this value never alters the media. Application admission is unlimited by
default (`RTMP_ADMISSION_MODE=unlimited`), so the calculated value is not an
artificial user cap. Use `RTMP_ADMISSION_MODE=capacity` only when the link
estimate should also be enforced as a hard admission limit. An advanced
operator can change the sizing floor, or supply a numeric
`RTMP_EXPECTED_STREAM_MBIT` as a capacity override. The relationship is:

```text
viewer budget = bandwidth × utilization ÷ (per-viewer bitrate × protocol overhead)
```

The default high-density target uses 90% link utilization and 8% overhead;
both are configurable. Fair egress shaping is enabled by default so the queue
stays on the VPS, existing viewers cannot consume the last part of the uplink,
and a new viewer's playlist/first segment receives a fair turn. It uses CAKE
through 10 Gbps and HTB plus `fq` above it. Set the provider's real committed
rate with `RTMP_BANDWIDTH_MBIT`; a virtual NIC's displayed speed may be higher.
`RTMP_ENABLE_FAIR_QUEUE=0` is available only when external shaping already
provides equivalent headroom and per-flow fairness.

The detected/declared bandwidth and auto-bitrate mode are written to the
installer-owned `runtime-config.json`, so the admin dashboard loads the server
link automatically instead of starting from a hard-coded UI value.

After installation:

- Install details: `/root/streamforge-credentials.txt`
- Admin panel: `http://VPS_IP` without a domain, otherwise `https://RTMP_DOMAIN`
- RTMP origin: `rtmp://VPS_IP:1935` without a domain, otherwise `rtmp://RTMP_DOMAIN:1935`
- Service logs: `journalctl -u rtmp-server -f`

## Operator flow

1. Open the admin panel directly; no sign-in is required.
2. Create an application such as `live`.
3. Create a stream such as `main-stage`.
4. Publish to the stream's RTMP URL:

   ```text
   rtmp://stream.example.com:1935/live/main-stage
   ```

5. Direct RTMP players may use that same URL. For many viewers, use the
   segmented playback URL shown by the panel:

   ```text
   https://stream.example.com/hls/live/main-stage/index.m3u8
   ```

   HLS segments are generated in C++ from the RTMP H.264/AAC ingest without
   re-encoding and require no token. If an encoder UI forces separate
   fields, split the same URL at the final slash (`Server:
   rtmp://stream.example.com:1935/live`, `Stream Key: main-stage`); this does
   not create a second server-side URL.

## Optional HLS delivery features

All three are off or free by default; nothing below changes an existing
deployment unless it is switched on in `server.yaml`. See `docs/hls.md`.

| Feature | Setting | What it buys |
|---|---|---|
| Low-Latency HLS | `hls_low_latency: true` | Each segment is published in `hls_part_target_duration` slices (`EXT-X-PART`) and playlist reloads block until the live edge reaches the position the player asked for, so latency drops from roughly three segment durations to roughly one part. The origin holds a blocked request without holding a thread. |
| AES-128 segment encryption | `hls_encryption_enabled: true` | `EXT-X-KEY` whole-segment AES-128-CBC. A leaked segment URL is worthless without a key fetched from `key-<id>.bin`, behind the same authorisation as the playlist. `hls_key_rotation_interval` bounds a leaked key's reach. Ciphertext is identical for every viewer, so segments stay fully cacheable. |
| Trick play | `hls_iframe_playlists: true` (default) | `iframe.m3u8` per stream, advertised from the master playlist, so a player can scrub without downloading whole segments. The byte ranges are recorded as a side effect of packaging and cost nothing when unused. |
| MPEG-DASH | `dash_enabled: true` | Packages the same publish into fMP4/CMAF and serves an MPD at `/dash`, in parallel with `/hls`. Doubles per-publisher packaging/storage cost, so it is off by default. See `docs/dash.md`. |

Low latency raises origin request volume (one cached object per part plus a
held connection per waiting player) and `hls_blocking_reload_timeout` must
stay below the cache tier's own backend timeout. Encryption and trick play are
mutually exclusive on the same rendition: an encrypted segment's byte range
would point into ciphertext, so the trick-play entry is dropped rather than
made wrong.

## Capacity reality

Delivery is bounded by the lowest of usable NIC bandwidth, CPU/kernel
send cost, memory/socket queues, and packets per second. A 10 Gbps port cannot
serve 10,000 viewers at 2.5 Mbps each: even before overhead that would require
25 Gbps. The admin panel includes a no-CDN capacity calculator, and the repo
includes `rtmp_load_gen` for measurement on the actual VPS. The playback hot
path reuses one immutable RTMP wire buffer across matching viewers and uses
capability-gated Linux SEND_ZC for large writes. HLS keeps a bounded live
window in RAM and routes shared public playlists/segments through local
Varnish; no external CDN is required. This removes per-viewer work inside the
C++ origin, but it cannot reduce the VPS's bytes sent on its physical NIC; see
`docs/high-density-rtmp.md` and `docs/hls.md`.

Examples with the installer's default 90% utilization and 5% overhead:

| Committed bandwidth | Average per viewer | Calculated viewer budget |
|---:|---:|---:|
| 50,000 Mbps | 1.00 Mbps | 42,857 |
| 50,000 Mbps | 0.85 Mbps | 50,420 |
| 60,000 Mbps | 0.85 Mbps | 60,504 |

The installer also scales the process connection ceiling, per-worker receive
buffer pool and systemd file-descriptor limit from the pre-live safety budget.
The dashboard's live viewer estimate uses the current measured bitrate, not
that sizing floor.

## Development build

Portable core build (also works on macOS):

```bash
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

Production Linux build:

```bash
cmake --preset production
cmake --build --preset production
```

Admin panel:

```bash
cd admin
npm ci
npm run dev
```

Append `?demo=1` to the local panel URL for the built-in safe preview dataset.

## Configuration and operations

- Example configuration: `config/server.example.yaml`
- Management API: `docs/management-api.md`
- Deployment, backup and rollback: `docs/deployment.md`
- Architecture: `docs/architecture.md`
- Load testing: `scripts/load-test.sh`

The management API binds to loopback. Caddy is the only public HTTP entry
point and proxies `/api/*`; the RTMP listener remains a direct TCP service on
port 1935.

## Current scope

Implemented: RTMP handshake/chunk/AMF0 pipeline, open-name publish/play
fan-out, GOP cache, slow-viewer backpressure, multi-worker routing, SQLite
control state, open management HTTP API, metrics, audit records, and the
Linux deployment stack.

Also implemented: FFmpeg-free transcoding, in two shapes
(`docs/native-transcoding.md`). Source-transcode jobs pull an external
`rtmp://`/HLS/HTTP-TS URL, transcode it in-process into an H.264/AAC rendition
ladder, and serve it as one adaptive master `.m3u8`. Ingest transcoding does
the same for a stream published *to* this origin: a per-stream transcoding
assignment (`/v1/transcoding/assignments`) turns one publish into a rendition
ladder served from `/hls/<app>/<stream>/master.m3u8`, while the untranscoded
passthrough playlist and RTMP playback stay exactly as they were. There is no
admin-panel page for assignments yet; the API is the interface.

Also implemented: scale-out beyond one box (`docs/clustering.md`). A publish
can be relayed over RTMP to a second origin, which then serves its own edge
tier, and the same mechanism pushes a stream to an external ingest (YouTube,
Facebook, a CDN) as a stream target. A stream can also name a backup RTMP
source that takes over packaging — reusing the exact same HLS/DASH/transcode
sink a real publish gets — when its primary publisher has been absent past a
configured grace period, and stands down automatically once the primary
returns. A cluster node registry tracks origins, edges, shields and
transcoders by heartbeat; `/v1/cluster/locate` answers which node a new viewer
should be sent to (region, then edge-over-origin, then least loaded), and
`/v1/cluster/redirect` acts on that answer directly with an HTTP 302, turning
placement into one-hop routing with no separate load balancer required for a
small deployment. `/v1/cluster/capacity` reports aggregate edge utilisation
and a scale-out recommendation for an external autoscaler to poll — this
process only reports the signal, it does not provision anything itself. See
`docs/transcoder-dispatch.md` for running dedicated pull/transcode/push worker
nodes behind that registry.

Not included: WebRTC, SRT, multi-node state replication, leader election or
origin failover (lose the origin process and you lose the control plane —
backup publisher failover covers a dead *source*, not a dead origin),
automated provisioning, or a CDN. HLS and recording components exist in the library but
their full production service composition is still separate from the main
direct-RTMP deployment; the panel persists recording policy but the installer
leaves global recording disabled.
