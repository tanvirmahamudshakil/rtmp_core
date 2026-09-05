# High availability: replication, failover, autoscale, transcoder offload

`docs/clustering.md` covers ingest fan-out (relay), backup ingest sources,
and viewer placement. This document covers the four pieces that make the
**control plane itself** survive a box dying, and the two pieces that turn
node placement into hands-off elastic capacity.

Read the whole document before deploying any of this. Every mechanism here is
built without a dedicated consensus store (no etcd/Consul/Postgres cluster
required) by design — that keeps the operational footprint small, but it is a
real trade-off, stated plainly in "What this is not" at the end.

## 1. Multi-node state replication

The control-plane database (applications, streams, transcoding assignments,
stream targets, backup-publisher configs, cluster nodes) is one SQLite file.
[Litestream](https://litestream.io) tails its WAL and streams every commit to
an S3-compatible bucket continuously.

```text
origin (leader)  --SQLite WAL-->  litestream replicate  --continuous-->  S3 bucket
                                                                              │
origin (standby) <--litestream restore (on promotion only)------------------┘
```

Setup: [litestream.yml.example](../deploy/ha/litestream.yml.example) +
[litestream.service](../deploy/ha/litestream.service). Any S3-compatible
store works — AWS S3, MinIO, Cloudflare R2, Backblaze B2, Wasabi.

This is **not** a multi-writer database and does not by itself make anything
"HA" — it is the write path leader/standby failover (below) is built on.
Litestream's own restore is a point-in-time recovery operation: a standby's
job is to run it fresh at the moment it becomes leader, not to keep
continuously replaying it in the background (litestream is not a hot-standby
replication protocol like Postgres streaming replication; using it as one
would burn S3 requests for no benefit here).

## 2. HA / automatic failover

[ha-agent.sh](../deploy/ha/ha-agent.sh) runs on every candidate origin and
implements lease-based leader election directly on top of the same bucket:

```text
┌─────────────┐   holds lease, renews every 5s   ┌──────────────┐
│  origin-a   │◄─────────────────────────────────┤  S3 lease    │
│  (leader)   │                                   │  object      │
│  serving    │                                   │ (TTL 15s)    │
└─────────────┘                                   └──────┬───────┘
                                                          │ polls every 3s
┌─────────────┐                                          │
│  origin-b   │◄─────────────────────────────────────────┘
│  (standby)  │   lease expired → restore latest replica → promote
│  idle       │
└─────────────┘
```

- **Detection is the lease TTL.** A leader that stops renewing (crashed, or
  partitioned from the bucket) has its lease expire in 15 s by default; a
  standby polling every 3 s notices within seconds.
- **Promotion always restores fresh.** The newly leading node deletes any
  local database file and runs `litestream restore` before starting
  `rtmp-server`, so it never starts from a stale or (after a fenced
  split-brain) suspect local copy.
- **Split-brain guard, not split-brain proof.** A leader that fails to renew
  self-fences (stops `rtmp-server` and `litestream` immediately) rather than
  assuming it is still fine. This depends on synchronized clocks (run
  NTP/chrony on every candidate) and on the object store correctly honouring
  conditional writes (`If-Match`/`If-None-Match`) — most S3-compatible stores
  do as of 2024, but verify yours before relying on this in production.
- **VIP/DNS is a pluggable hook**, not built in — [hooks/on-promote.example.sh](../deploy/ha/hooks/on-promote.example.sh)
  shows a Cloudflare DNS update; a floating IP re-association or a keepalived
  priority change fits the same hook point. This is deliberately left
  provider-specific, the same posture as the autoscaler's provisioning hooks
  below.

Bring-up: `STREAMFORGE_HA_BOOTSTRAP_LEADER=1` on exactly one candidate, exactly
once, when the bucket is empty. Every failover after that is automatic.

### What actually resumes after a failover, and what does not

| | Resumes automatically |
|---|---|
| Applications, streams, transcoding assignments, stream targets, backup-publisher configs, cluster node table | Yes — all control-plane state, via the restored database |
| Whoever is currently publishing | **No.** An encoder must reconnect to the (possibly new) address; this is identical to any RTMP origin restart. Configure the encoder to retry, or point it at the DNS name the on-promote hook updates. |
| HLS live windows on edges | Unaffected — edges cache independently and keep serving stale-while-revalidate until the reconnect. |
| In-flight recordings | Finalized wherever they were running; a new one starts when the publisher reconnects to the new leader. |

## 3. Auto-provisioning (scale signal → hooks, not a cloud SDK)

[autoscaler.sh](../deploy/ha/autoscaler.sh) polls the origin's
`/v1/cluster/capacity` and `/v1/cluster/nodes` and calls **pluggable
hooks** to scale — it never talks to a cloud API itself, because there is no
single API to talk to across AWS/Hetzner/DigitalOcean/bare metal. This is the
same shape every provider-agnostic autoscaler takes: real decision logic,
provider-specific action delegated to a script you write once for your
environment.

```text
autoscaler.sh (runs anywhere reachable — the origin itself, or a small
               control host)
    │  poll /v1/cluster/capacity every 30s
    ▼
utilization >= 85% for 3 consecutive polls
    │
    ▼
hooks/provision-edge  (you write this: call your cloud API,
                        boot a VM, cloud-init runs install-edge.sh,
                        it self-registers by heartbeat)
```

```text
utilization <= 40% for 10 consecutive polls, and > 1 healthy edge
    │
    ▼
PATCH /v1/cluster/nodes/<id> {"draining":true}   (origin API, new — see below)
    │  wait until active_viewers reaches 0 (or a timeout)
    ▼
hooks/deprovision-edge <node-id>   (you write this: terminate the VM)
    │
    ▼
DELETE /v1/cluster/nodes/<id>
```

**`draining` can now be set from the origin**, not only by the node itself:
`PATCH /v1/cluster/nodes/<id>` with `{"draining": true|false}` sets a
control-plane override that is OR'd with whatever the node reports in its own
heartbeat. This is what lets an autoscaler (which usually cannot reach an
edge directly — no SSH access from a cloud API's perspective) drain a node
before terminating it, without needing to log into the edge at all.

```bash
curl -X PATCH localhost:8080/v1/cluster/nodes/edge-eu-3 -d '{"draining":true}'
```

Hooks live in a directory you point `STREAMFORGE_AUTOSCALE_HOOKS_DIR` at,
named `provision-edge` and `deprovision-edge`; each receives the target
region as `$1` (provision) or the node id as `$1` (deprovision). A repo
without cloud credentials cannot ship a working `provision-edge` for you —
the honest deliverable here is the polling/decision loop and the two call
sites, not a fake implementation for a provider you may not use.

**"Warm pool" alternative, no cloud API needed at all:** if `provision-edge`
instead just SSHes into a pre-booted, already-provisioned-but-idle machine
and starts its `streamforge-node-heartbeat.timer` + Varnish/Caddy (both
already installed, just not yet serving), scaling out costs one SSH call and
a few seconds — no VM boot time, no cloud API, no credentials beyond SSH keys.
This is a legitimate, simpler pattern for a fixed-size fleet with headroom.

## 4. Transcoder tier remote dispatch

A source-transcode job (`docs/native-transcoding.md`) can now run on a
dedicated transcoder node instead of the origin box, so a heavy rendition
ladder never competes with the origin's own ingest/HTTP/admin threads for CPU.

```text
origin                              transcoder node (role=transcoder)
  │  POST job assignment                  │
  │ ───────────────────────────────────►  │  transcoder_agent
  │  (HTTP, bearer token)                 │    runs SourceJobManager locally
  │                                       │    (decode once, encode per rung —
  │                                       │     the exact same native pipeline
  │                                       │     as a local source job)
  │                                       │
  │  ◄─── RTMP push (RtmpPushClient) ─────┤  each rung published back to the
  │       one publish per rendition          origin under a reserved
  │                                          application, as though a real
  │                                          encoder had sent it
  │
  │  HLS/DASH packaging proceeds exactly
  │  as it would for any other publish
```

The agent does not serve HLS itself and is not a delivery destination (same
as a shield) — it decodes/encodes and pushes the result back over RTMP, reusing
[RtmpPushClient](../include/rtmp_server/relay/rtmp_push_client.hpp) exactly as
a stream target does. This means a transcoder node needs no public HTTP
surface at all: outbound RTMP to the origin is its only network requirement.

See `docs/transcoder-dispatch.md` for the wire protocol, the agent binary
(`apps/transcoder_agent`), and how to assign a job to a specific node versus
letting the origin place it on the least-loaded healthy transcoder.

## What this is not

- **No consensus system.** The lease lives in S3, not in a quorum-based store.
  A network partition that lets a fenced-but-still-running leader keep
  believing it holds the lease for longer than clock skew allows is the
  scenario a real consensus system (Raft/Paxos-backed etcd or Consul) is
  designed to close and this is not. If your uptime requirement justifies
  that engineering cost, use etcd/Consul-based leader election instead — the
  agent's promote/demote/hook logic is the part worth keeping either way.
- **Failover has a restore-time gap.** Promotion is not instant: it restores
  the database before starting. For this control-plane database (small — no
  media, just configuration and a heartbeat table), that is normally low
  single-digit seconds, but it is not zero.
- **The autoscaler does not provision anything by itself.** It is a decision
  loop with two call sites you fill in for your infrastructure.
- **Transcoder dispatch does not include scheduling policy beyond
  least-loaded-healthy-transcoder.** No node affinity, no GPU-awareness, no
  bin-packing across multiple concurrent jobs.
- **None of this replaces backups.** Litestream retention is not a substitute
  for testing restores; do that before you need it.
