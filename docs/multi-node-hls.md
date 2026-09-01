# Multi-node HLS: edges and origin-shield

A single StreamForge box already serves one HLS link to a large audience: the
co-located Varnish (`deploy/varnish/streamforge.vcl`) collapses every viewer's
playlist poll into roughly one origin request per second and serves immutable
segments from RAM. The ceiling on that box is its **uplink bandwidth**, not
CPU:

```
concurrent viewers  ≈  usable uplink Mbps  ÷  stream Mbps
```

To go past that ceiling — more total bandwidth, viewers on other continents,
resilience to one machine failing — add **edge** nodes. Each edge is a pure
cache (Varnish + Caddy, no `rtmp_server`, no database) that pulls from the
origin and serves its own slice of the audience. Edges scale linearly: double
the edges, double the deliverable bandwidth.

This is the same origin/edge model Wowza's live stream repeater uses; here the
transport is plain cacheable HTTP, so an "edge" is just a correctly configured
reverse cache.

## Topology

```
                    ┌────────────────────────────────────────────┐
   OBS / encoder ──▶│  ORIGIN                                     │
        (RTMP)      │  rtmp_server ─▶ Varnish :6081 ─▶ Caddy :443 │
                    │  (segments in RAM)   (built-in shield)      │
                    └───────────────▲────────────────────────────┘
                                    │  HTTPS, X-Edge-Token
                 ┌──────────────────┼───────────────────┐
                 │                  │                   │
          ┌──────┴─────┐     ┌──────┴─────┐      ┌──────┴─────┐
          │  EDGE eu-1  │     │  EDGE us-1  │      │  EDGE ap-1 │
          │ Caddy+Varnish│    │ Caddy+Varnish│     │Caddy+Varnish│
          └──────▲─────┘     └──────▲─────┘      └──────▲─────┘
                 │                  │                   │
             viewers            viewers             viewers   (HTTPS)
```

Each edge's local path:

```
viewer ──HTTPS──▶ Caddy :443 ──HTTP──▶ Varnish :6081 (streamforge-edge.vcl)
       ──HTTP──▶ Caddy :8090 ──HTTPS──▶ origin (or shield)
```

The inner Caddy hop terminates TLS to the upstream so Varnish uses a plain
HTTP backend — that is why `streamforge-edge.vcl` is byte-identical on every
node and needs no per-host templating.

### When to add a dedicated shield

The origin's own Varnish already acts as a shield: N edges fetching a given
object at once still produce **one** `rtmp_server` request. A separate shield
node only earns its keep once you run enough edges (rule of thumb: **> 8**, or
edges in many regions each doing frequent playlist refresh) that the origin
box's Varnish CPU or its NIC becomes the fan-in limit. A shield is just an
edge with `STREAMFORGE_ROLE=shield` and no viewers of its own; point the edge
fleet at the shield's `:6081` instead of at the origin.

## Security model

| Concern | Mechanism |
|---|---|
| Public bypassing edges to hit the origin directly | Origin sets `hls_edge_fetch_secret`; `/hls` is served only to requests with `X-Edge-Token: <secret>`. Every edge presents it in `vcl_backend_fetch`. |
| Origin used as an open segment proxy | Same token gate. |
| Edge serving anything but delivery | `streamforge-edge.vcl` `return (synth(403))` for any URL outside `/hls/`. The management API, `/metrics`, and admin panel never leave the origin's private network. |
| Token on the wire | Every hop is HTTPS (viewer→edge, edge→origin). The token rides an HTTPS request header, never a URL. |
| Shield's internal port exposed | `install-edge.sh` firewalls the Varnish port to `STREAMFORGE_EDGE_CIDRS` when `ROLE=shield`. |

The token is compared with `core::constant_time_equals`. Rotating it: set the
new value on the origin (`RTMP_SERVER_HLS_EDGE_FETCH_SECRET`) and every edge
(`STREAMFORGE_EDGE_TOKEN` in `/etc/systemd/system/varnish.service.d/streamforge-edge.conf`),
then `systemctl restart` each. During the overlap window the origin briefly
answers 403 to the not-yet-updated side; edges serve stale from cache
(`grace`/`keep`) so viewers see no interruption.

## Cache behaviour on an edge

`streamforge-edge.vcl` mirrors the origin's asymmetric TTLs and adds generous
stale windows so a momentarily unreachable origin never stalls playback:

| Object | TTL | grace (serve-stale-while-revalidate) | keep (stale-if-error) |
|---|---|---|---|
| media playlist `.m3u8` | 1s | 15s | 30s |
| `master.m3u8` | 30s | 5m | 10m |
| segment `.ts` | 1h | 24h | 24h |
| any non-200 | 1s | 2s | 0s |
| per-viewer 302 (`no-store`) | uncacheable (passed through) | — | — |

- Segment URLs have their query string stripped in `vcl_recv` (a `.ts` is
  immutable and uniquely named) to maximise the hit ratio.
- Request cookies are dropped — delivery objects are shared across viewers.
- A non-200 always gets a short **positive** TTL, never `uncacheable`, so a
  404/5xx burst is coalesced into one origin round trip instead of a
  thundering herd.
- `vcl_init` fails the VCL load if `STREAMFORGE_EDGE_TOKEN` is unset, so a
  misconfigured node fails loudly instead of serving nothing but 403s.

Every response carries `X-Cache: HIT|MISS`, `X-Cache-Hits`, and
`X-Edge-Role` / `X-Edge-Node` for debugging and per-node attribution.

## Directing viewers across edges

The origin returns HLS URLs built from its own hostname. For multi-node you
publish the link under a name that resolves to the edges, not the origin.
Options, cheapest first:

1. **Round-robin DNS** — multiple `A`/`AAAA` records for `cdn.example.com`,
   one per edge. Clients spread themselves; a dead edge still gets ~`1/N` of
   requests until TTL expiry, so keep the record TTL low (30–60s) and pair
   with health-checked DNS if the provider supports it.
2. **GeoDNS / latency-based DNS** (Route 53, NS1, Cloudflare LB, …) — resolve
   each viewer to its nearest healthy edge. This is the usual production
   choice.
3. **A single anycast/L4 load balancer** in front of the edges — one viewer
   IP, the LB spreads connections and drops unhealthy edges. Adds a hop and a
   component to run.
4. **A commercial CDN in front of the edges (or straight at the origin)** —
   the origin's headers are already CDN-correct (`s-maxage`, `immutable`,
   `no-store` on errors). If a CDN is acceptable, it replaces the edge tier
   entirely; the edge tier exists for deployments that must stay on
   self-managed infrastructure.

## Deploy

### 1. Enable the gate on the origin

Fresh install:

```bash
sudo env RTMP_DOMAIN=stream.example.com \
  RTMP_EDGE_TOKEN="$(openssl rand -hex 32)" \
  bash scripts/install-linux.sh
```

Existing install: add `RTMP_SERVER_HLS_EDGE_FETCH_SECRET=<token>` to
`/etc/rtmp-server/rtmp-server.env`, add
`set bereq.http.X-Edge-Token = "<token>";` to `vcl_backend_fetch` in
`/etc/varnish/streamforge.vcl`, then
`systemctl restart rtmp-server varnish`.

The token is printed in `/root/streamforge-credentials.txt`.

### 2. Add each edge

On a fresh Debian/Ubuntu host:

```bash
git clone https://github.com/tanvirmahamudshakil/rtmp_core.git && cd rtmp_core
sudo env \
  STREAMFORGE_ORIGIN=https://stream.example.com \
  STREAMFORGE_EDGE_TOKEN=<the token from step 1> \
  STREAMFORGE_DOMAIN=edge-eu-1.example.com \
  bash deploy/edge/install-edge.sh
```

The installer verifies the delivery path end to end before finishing:
`404` for a nonexistent stream = origin reachable and token accepted; `403` =
token mismatch; `502/503` = origin unreachable.

### 3. (Optional) Add a shield

```bash
sudo env \
  STREAMFORGE_ORIGIN=https://stream.example.com \
  STREAMFORGE_EDGE_TOKEN=<token> \
  STREAMFORGE_ROLE=shield \
  STREAMFORGE_EDGE_CIDRS="203.0.113.0/24,198.51.100.7/32" \
  bash deploy/edge/install-edge.sh
```

Then reinstall each edge with `STREAMFORGE_UPSTREAM=https://shield.example.com`.

### 4. Point viewer DNS at the edges

Per "Directing viewers across edges" above.

## Verifying

```bash
# From anywhere:
curl -sI https://edge-eu-1.example.com/hls/live/demo/index.m3u8 | grep -i 'x-cache\|x-edge'
#   X-Cache: HIT            <- second request within the TTL
#   X-Edge-Role: edge
#   X-Edge-Node: edge-eu-1

# On an edge:
varnishstat            # MAIN.cache_hit / MAIN.cache_miss ratio
varnishlog -g request  # per-request trace, backend fetches, grace hits
```

A healthy edge under load shows a cache hit ratio well above 95% (every
segment after the first viewer, every playlist poll inside the 1s window).

## Failure modes

| Event | Result |
|---|---|
| One edge dies | Its viewers re-resolve (DNS) or the LB removes it. Other edges unaffected. |
| Origin briefly unreachable | Edges serve stale playlists for `grace` (15s) and stale segments for `keep` (24h). Playback continues. |
| Origin down longer than `grace` | New joins fail; in-progress players stall when their buffered window drains. Segments already cached still play. |
| Token mismatch after a botched rotation | Origin returns 403; edges serve stale until `keep` expires. Fix the token and `systemctl restart varnish` on the affected side. |
| Edge disk/RAM cache full | Varnish LRU-evicts the coldest objects (`nuke_limit`). Live objects are tiny and hot, so this only touches long-tail history. |

## Observability across edges

Each edge runs its own `varnishncsa`; there is no single aggregated HLS viewer
count across the fleet yet (`edge_viewer_stats_path` on the origin reads one
file from one co-located estimator). Until fleet aggregation lands, per-edge
`varnishstat` / `varnishncsa` and the origin's own `/metrics`
(`hls_active_viewers` from the local estimator, RTMP counters) are the
sources. A simple approach that works today: run `viewer-estimator` on each
edge writing to shared storage (or pushed to the origin) and sum the
per-stream counts externally.
