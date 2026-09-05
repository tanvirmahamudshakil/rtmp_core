# Dedicated transcoder nodes

StreamForge can keep CPU-heavy decoding and encoding off the RTMP origin. The
origin owns job configuration and HLS/DASH packaging; a `transcoder_agent`
pulls the source, decodes it once, encodes every rendition, and publishes each
rendition back to the origin over RTMP.

```text
source RTMP -> transcoder_agent -> one RTMP publish per rendition -> origin -> HLS/DASH
```

The agent does not serve viewers, package segments, or persist control-plane
state. If it restarts, the origin's persisted job is dispatched again after
the node resumes heartbeating.

## Build and run

The executable is built only when the native codec dependencies are present.
Install the packages listed in [native-transcoding.md](native-transcoding.md),
configure a normal Linux build, then install or copy `transcoder_agent` to the
worker host.

The listener is configured with environment variables:

| Variable | Default | Meaning |
| --- | --- | --- |
| `RTMP_TRANSCODER_BIND` | `0.0.0.0` | Job API bind address |
| `RTMP_TRANSCODER_PORT` | `9200` | Job API port; must match the origin's dispatch port |
| `RTMP_TRANSCODER_MAX_JOBS` | `8` | Hard concurrent-job ceiling |

The API exposes `GET /health/live`, `GET /health/ready`, `GET /jobs`,
`POST /jobs`, and `DELETE /jobs/<application>/<name>`. Assignment POSTs are
idempotent: an identical retry keeps the current media pipeline; changed
configuration stops and replaces it. Bodies are capped at 1 MiB and a job may
contain at most 16 renditions.

## Register the node

The origin only dispatches to healthy nodes with role `transcoder`. Run
`deploy/edge/streamforge-node-heartbeat.sh` every ten seconds with the
variables in `deploy/transcoder/transcoder-agent.env.example`. For transcoder
nodes, the generic cluster `capacity_viewers` and `active_viewers` wire fields
represent job slots and active jobs; this lets the existing least-loaded node
selection balance work without a second capacity schema.

The example systemd service and timer under `deploy/transcoder/` expect:

- `transcoder_agent` at `/usr/local/bin/transcoder_agent`;
- the heartbeat script at
  `/usr/local/lib/streamforge/streamforge-node-heartbeat.sh`;
- environment at `/etc/streamforge/transcoder-agent.env`.

Keep TCP 9200 on a private network/firewall allow-list reachable only by
origins. The agent protocol is intentionally plain HTTP and has no public
authentication layer; it shares the trusted cluster-network boundary used by
node heartbeats. The worker also needs outbound access to the source RTMP URL
and the origin's RTMP listener.

## Assignment body

The origin sends this JSON shape:

```json
{
  "id": "live/main",
  "source_url": "rtmp://source.internal/live/input",
  "fps": 30,
  "target_application": "live",
  "origin_rtmp_host": "origin.internal",
  "origin_rtmp_port": 1935,
  "renditions": [
    {
      "name": "720p",
      "output_stream": "main_720p",
      "width": 1280,
      "height": 720,
      "video_bitrate": 2500000,
      "audio_bitrate": 128000
    }
  ]
}
```

Each encoder output is converted from Annex B/ADTS into ordinary RTMP
AVC/AAC tags. Sequence headers are sent before the first media frame and are
replayed whenever a push reconnects, so a recovered origin connection starts
on a decodable boundary.

Graceful shutdown stops accepting requests, cancels every pull/push loop,
closes its bounded media queues, and joins all job threads. Origin-side jobs
remain persisted and will be reassigned when a healthy agent is available.
