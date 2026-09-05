# Clustering and scale-out

A single StreamForge box ingests, packages and delivers on its own. This
document covers what to do when one box is not enough — and, just as
importantly, what this deployment still cannot do.

Three mechanisms exist, each solving a different ceiling:

| Ceiling you hit | Mechanism | Where |
|---|---|---|
| Delivery bandwidth | HTTP cache edges in front of the origin | [multi-node-hls.md](multi-node-hls.md) |
| Ingest / fan-out capacity, or reaching another platform | **RTMP relay + stream targets** | this document |
| A publisher's primary encoder/link dying | **Backup publisher failover** | this document |
| Knowing which nodes exist and where a viewer should go, and routing to it | **Cluster node registry + redirect** | this document |
| Deciding *when* to add edge capacity | **Capacity/scale-out signal** | this document |

## Stream targets and relays

One publish can be pushed back out over RTMP to any number of destinations:

```text
OBS ──RTMP──▶ origin ──┬──▶ HLS/DASH viewers (via the edge tier)
                       ├──▶ rtmp://origin-2.internal/live/key      (relay)
                       ├──▶ rtmp://a.rtmp.youtube.com/live2/<key>  (stream target)
                       └──▶ rtmp://live-api.facebook.com/rtmp/<key>
```

A **relay** target points at another StreamForge origin, which then serves its
own viewers from its own edge tier — this is the equivalent of Wowza's live
stream repeater, and it is how ingest and fan-out scale past one machine. A
**stream target** points at an external ingest. The wire behaviour is
identical; `relay: true` only changes how the target is reported, so an
operator can tell capacity apart from distribution.

```bash
# Relay this publish to a second origin.
curl -X PUT localhost:8080/v1/stream-targets/live:main-stage:origin-2 \
  -d '{"url":"rtmp://origin-2.internal:1935/live/main-stage","relay":true}'

# Push the same publish to an external ingest.
curl -X PUT localhost:8080/v1/stream-targets/live:main-stage:youtube \
  -d '{"url":"rtmp://a.rtmp.youtube.com/live2/xxxx-xxxx-xxxx-xxxx"}'

curl 'localhost:8080/v1/stream-targets?application=live'
curl -X PATCH localhost:8080/v1/stream-targets/live:main-stage:youtube -d '{"enabled":false}'
curl -X DELETE localhost:8080/v1/stream-targets/live:main-stage:youtube
```

Targets are stored in the `stream_targets` table and reloaded at startup.
There is no admin-panel page for them yet; the API is the interface.

### How a target behaves

**The publish is never blocked.** Media is copied into a bounded queue and
leaves on the target's own thread ([media_handoff_queue.hpp](../include/rtmp_server/media/media_handoff_queue.hpp)).
A target that is slow or dead loses its own frames — mid-GOP video first, then
everything until the next keyframe — while ingest, HLS packaging and every
other target continue untouched.

**A target's URL is a credential.** Its last path segment is the destination's
stream key, so the API returns it redacted (`.../live2/****xxxx`), the audit
log records which target changed but never its URL, and the raw value exists
only in the database and in the outbound connection.

**Reconnects are automatic and never give up.** A dropped ingest (routine on
the large platforms) is retried with the configured delay doubling per
consecutive failure up to 60 s, then held there indefinitely. Every attempt
starts by replaying the cached metadata and AVC/AAC sequence headers, then
waits for a keyframe, so the far side always receives a decodable stream
rather than the middle of a GOP. A push that held for a minute or more resets
the streak, so a long-lived target reconnects at the configured delay rather
than at the cap.

**Timestamps are re-based.** The target sees a stream starting near zero
however long this origin has been publishing, which several ingests require.

**Fan-out is bounded** at 8 targets per stream: each one is a full outbound
copy of the publish.

### Relay topology

```text
                 publishers
                     │ RTMP
              ┌──────▼───────┐
              │  origin A    │───── HLS ───▶ edge tier A ──▶ viewers
              │ (control     │
              │  plane)      │──RTMP relay──┐
              └──────────────┘              │
                                     ┌──────▼───────┐
                                     │  origin B    │── HLS ─▶ edge tier B ─▶ viewers
                                     └──────────────┘
```

Origin B is an ordinary StreamForge install: the relay arrives as a normal
publish, so it packages HLS, can carry its own transcode assignments, and
serves its own edges. Create the application and stream on origin B first —
publishing is name-validated, so an unknown stream is refused.

## Backup publisher failover

A stream can name a fallback RTMP source that takes over packaging the moment
its primary publisher has been absent for a configured grace period — Wowza's
"backup ingest point", without needing a second physical publisher connection:
the backup is *pulled*, not pushed.

```bash
curl -X PUT localhost:8080/v1/backup-publishers/live:main-stage \
  -d '{"backup_url":"rtmp://redundant-encoder.internal:1935/live/main-stage","failover_after_seconds":15}'

curl 'localhost:8080/v1/backup-publishers?application=live'
curl -X DELETE localhost:8080/v1/backup-publishers/live:main-stage
```

Rows persist in `backup_publishers` and reload at startup.

### How it behaves

**It reuses the real publish path.** The moment the backup activates, this
server builds the *exact* HLS/DASH/transcode/target sink a live publisher on
that stream would get — the same `recorder_factory` — so a failover produces
identical output with no separate packaging code path to keep in sync. The
backup source is plain RTMP, so its `RtmpMessage`s are forwarded to that sink
untouched — no decode/re-encode, unlike the ingest transcode ladder.

**The grace period protects against routine reconnects.** A publisher that
drops for a few seconds (common on mobile encoders) must not trigger a
failover; `failover_after_seconds` (default 15) is how long the primary may be
absent before the backup is dialed.

**Recovery is automatic.** The moment the primary is observed live again, the
backup connection is torn down and its sink finalized — the primary resumes
packaging on its own next segment.

**A failed backup keeps retrying**, same bounded-backoff pattern as a stream
target, for as long as the primary stays absent.

## Cluster node registry

Every node except the origin is stateless, so membership is a soft registry:
nodes announce themselves by heartbeat and disappear by going quiet.

```bash
curl localhost:8080/v1/cluster/nodes                      # what is running
curl 'localhost:8080/v1/cluster/locate?region=eu'         # where a viewer should go
curl -X DELETE localhost:8080/v1/cluster/nodes/edge-eu-1  # decommission
```

The origin heartbeats itself every 10 s. Edges and shields heartbeat from
`streamforge-node-heartbeat.timer`, installed by
[install-edge.sh](../deploy/edge/install-edge.sh) whenever
`STREAMFORGE_MANAGEMENT_URL` is set (it defaults to `STREAMFORGE_ORIGIN`).
Identity on the origin comes from the environment — `STREAMFORGE_NODE_ID`,
`STREAMFORGE_NODE_REGION`, `STREAMFORGE_NODE_ADDRESS` — because an id that
differs per machine does not belong in a `server.yaml` that gets copied
between them.

`/v1/cluster/locate` is the placement rule, in order:

1. skip anything unhealthy (no heartbeat for 30 s), draining, or at its
   reported viewer ceiling;
2. skip shields and transcoders — neither is a delivery destination;
3. prefer the requested region;
4. prefer an edge over an origin (sending viewers to the box that also ingests
   and packages is exactly what the edge tier exists to prevent);
5. among equals, take the lowest `active_viewers / capacity_viewers`.

It answers `503 no_node_available` when nothing can take a viewer, which is
what an external load balancer or redirector needs to see. A node silent for
24 h is forgotten entirely.

### Real routing, not only an answer

`/v1/cluster/locate` returns the *decision* (which node) for a balancer or
panel to act on. `/v1/cluster/redirect` goes one step further and **acts on
it**: it runs the same placement, then answers with an HTTP 302 straight to
the chosen node's playback URL.

```bash
curl -i 'localhost:8080/v1/cluster/redirect/live:main-stage?region=eu'
# HTTP/1.1 302 Found
# Location: https://edge-eu-1.example.com/hls/live/main-stage/index.m3u8
```

Point a player, or a thin DNS/anycast-fronted redirector, at this route and
placement becomes real traffic routing with one extra hop — no load balancer
product required for a small deployment. `format=dash` redirects to the MPD
instead. A 503 here means what it means everywhere else in this API: nothing
healthy could take the viewer.

### Scale-out signal

```bash
curl localhost:8080/v1/cluster/capacity
# {"healthy_edges":3,"capacity_viewers":45000,"active_viewers":39200,
#  "utilization":0.871,"scale_out_recommended":true}
```

Aggregate edge capacity and utilisation, with `scale_out_recommended` set past
85% (configurable). This is a **signal an external autoscaler polls** —
nothing in this process provisions, boots or removes a node itself. Wiring it
to an actual scaler (a cloud provider's instance group, a Terraform run, a
Kubernetes HPA-style controller) is deployment-specific and left to the
operator; this table is the input such a controller needs.

## What this is not

Be clear about the gap before designing around it.

- **There is no consensus, no leader election and no state replication.** The
  origin owns the database. Lose the origin and you lose the control plane —
  publishers, the panel, and every API above. Edges keep serving their caches
  until the live window drains, and a relay origin keeps serving whatever is
  still being pushed to it, but nothing promotes itself.
- **There is no auto-scaling.** `/v1/cluster/capacity` gives a real
  utilisation signal, but nothing here provisions, boots or destroys a node —
  that action belongs to your cloud provider's tooling, wired to poll this
  endpoint.
- **`locate` is advisory; `redirect` is real but still just one hop.** Nothing
  here is a full L4/L7 load balancer (health-checked backend pools, connection
  draining, TLS termination at scale) — for that, put a real LB in front and
  have it poll `/v1/cluster/nodes`/`/v1/cluster/capacity` instead.
- **A relay is not origin failover.** If origin A dies, origin B keeps
  whatever it had until the publisher reconnects — to A. (Backup publisher
  failover, above, solves the *publisher's own* primary source dying — a
  redundant encoder or contribution link — not the origin process dying.)
- **The cluster table is not authenticated separately.** It sits behind the
  same open management API as everything else (see the management API's own
  security note), so the API must not be exposed publicly.

## Ports

| From | To | Port | Why |
|---|---|---|---|
| origin | relay origin | 1935/tcp | the RTMP push |
| edge/shield | origin | 443/tcp | segment fetch **and** the heartbeat |
| operator | origin | management API (loopback + Caddy) | cluster, target and backup-publisher routes |
| viewer / player | origin | management API (loopback + Caddy) | `/v1/cluster/redirect` |
| origin | backup source | 1935/tcp | backup publisher failover |
