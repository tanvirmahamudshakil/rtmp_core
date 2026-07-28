# StreamForge RTMP Server

A self-hosted C++23 RTMP origin for Linux, built on `io_uring`, with a secure
web control plane. It accepts OBS/encoder publishing, validates hashed stream
keys, fans H.264/AAC RTMP media to viewers, persists application and stream
configuration in SQLite, and exports Prometheus metrics.

This deployment is designed for direct origin delivery. It does not require
or configure a CDN.

## One-command Linux installation

Ubuntu 24.04+ or Debian 13+ with systemd:

```bash
git clone https://github.com/tanvirmahamudshakil/rtmp_core.git
cd rtmp_core
sudo env RTMP_BANDWIDTH_MBIT=auto bash scripts/install-linux.sh
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
detects the public network interface, and runs a readiness check.

`RTMP_BANDWIDTH_MBIT=auto` reads the primary NIC's reported link speed using
Linux sysfs/ethtool. On virtual VPS interfaces that number can be higher than
the bandwidth purchased from the provider, so set the committed Mbps
explicitly whenever it differs.

Stream bitrate is automatic by default. The server is a media passthrough: it
does not re-encode or force a bitrate, so viewers receive the bitrate sent by
OBS or an upstream transcoder. After publishing starts, the admin panel first
uses measured egress per active viewer; before a viewer connects, it uses
measured ingress per active publisher. The live estimate refreshes with the
server metrics.

Before the first publisher connects there is no bitrate to measure, but the
installer still has to create finite socket, buffer and file-descriptor safety
limits. It uses `RTMP_RESOURCE_SIZING_MBIT=0.50` only for that pre-live
resource ceiling; this value never alters the media. An advanced operator can
change that sizing floor, or supply a numeric `RTMP_EXPECTED_STREAM_MBIT` as a
backward-compatible capacity override. The capacity relationship is:

```text
viewer budget = bandwidth × utilization ÷ (per-viewer bitrate × protocol overhead)
```

The default high-density target uses 90% link utilization and 5% overhead;
both are configurable. The network tune uses CAKE at 95% of the declared
uplink through 10 Gbps; above that it switches to lower-overhead Linux `fq`
per-flow pacing so the queue discipline does not become the throughput
bottleneck.

The detected/declared bandwidth and auto-bitrate mode are written to the
installer-owned `runtime-config.json`, so the admin dashboard loads the server
link automatically instead of starting from a hard-coded UI value.

After installation:

- Admin credential: `/root/streamforge-credentials.txt`
- Admin panel: `http://VPS_IP` without a domain, otherwise `https://RTMP_DOMAIN`
- RTMP origin: `rtmp://VPS_IP:1935` without a domain, otherwise `rtmp://RTMP_DOMAIN:1935`
- Service logs: `journalctl -u rtmp-server -f`

## Operator flow

1. Sign in to the admin panel with the generated bearer token.
2. Create an application such as `live`.
3. Create a stream and copy its one-time publish key.
4. Configure OBS:

   ```text
   Service:    Custom
   Server:     rtmp://stream.example.com:1935/live
   Stream Key: <one-time key from the panel>
   ```

5. Play the public stream:

   ```text
   rtmp://stream.example.com:1935/live/STREAM_NAME
   ```

## Capacity reality

Direct delivery is bounded by the lowest of usable NIC bandwidth, CPU/kernel
send cost, memory/socket queues, and packets per second. A 10 Gbps port cannot
serve 10,000 viewers at 2.5 Mbps each: even before overhead that would require
25 Gbps. The admin panel includes a no-CDN capacity calculator, and the repo
includes `rtmp_load_gen` for measurement on the actual VPS.

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

Implemented: RTMP handshake/chunk/AMF0 pipeline, publish/play fan-out, GOP
cache, slow-viewer backpressure, multi-worker routing, hashed publish keys,
signed playback tokens, SQLite control state, management HTTP API, metrics,
audit records, and the Linux deployment stack.

Not included: transcoding, WebRTC, SRT, clustering, multi-node state
replication, or a CDN. HLS and recording components exist in the library but
their full production service composition is still separate from the main
direct-RTMP deployment; the panel persists recording policy but the installer
leaves global recording disabled.
