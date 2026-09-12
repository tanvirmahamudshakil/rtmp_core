# High-density RTMP delivery

The production RTMP path is optimized for one publisher feeding many direct
TCP viewers. It is still unicast: 1,000 viewers mean 1,000 sockets and roughly
1,000 times the source bitrate in outbound traffic.

## Hot path

For playback media, the server now:

1. keeps the ingested payload in one immutable `SharedMediaFrame`;
2. encodes a stateless, always-valid RTMP fmt0 wire buffer once per
   `(frame, chunk size, chunk-stream ID, message-stream ID)`;
3. serves the common cached wire representation through an immutable,
   lock-free read path and shares that allocation across every matching
   viewer queue, including viewers on other io_uring workers;
4. uses `IORING_OP_SEND_ZC` for buffers of at least 16 KiB when both config
   and the running kernel support it;
5. retains the source allocation through the SEND_ZC notification CQE, then
   releases it; and
6. disables zero-copy and retries with ordinary io_uring send if the
   kernel/NIC rejects the operation at runtime.

Small command/audio writes continue through ordinary io_uring send because
zero-copy setup and notification overhead is not worthwhile for small
buffers. Every connection still has byte and packet queue caps, and the
keyframe-aware slow-viewer policy prevents a slow socket from growing memory
without bound.

Cross-worker media queues signal their destination only when changing from
empty to non-empty. Further frames in the same burst reuse that wakeup, and a
bounded drain re-signals if work remains. This removes one `eventfd` syscall
per destination per frame without adding polling latency. Fan-out also reuses
its delivery records for post-callback backpressure results instead of
allocating a second vector for every frame.

The VPS installer sets a 4 MiB / 512-message slow-viewer queue. At common live
bitrates this absorbs several seconds of jitter while bounding worst-case RAM.
Varnish cache HITs remain unconstrained by origin concurrency; only concurrent
cache MISSes are capped to the origin's CPU-sized HTTP pool, preventing a
restart/join stampede from starving current playlists or media ingest.

Always-fmt0 media adds a few RTMP header bytes per frame. That deliberate
trade saves a payload-sized allocation/copy and stateful encode for every
viewer. Command and protocol-control traffic retains normal fmt0-3 header
compression on its connection-local chunk streams.

## Configuration

`enable_send_zero_copy: true` is capability-gated. It is safe to leave enabled
on a kernel without SEND_ZC: the server uses ordinary io_uring send.

The primary capacity controls remain:

- `maximum_connections`
- `maximum_viewers_per_stream`
- `worker_ring_count`
- `subscriber_queue_max_bytes`
- `subscriber_queue_max_packets`
- `provided_buffer_count`

Configured limits are safety ceilings, not performance claims.

### Transport tuning

Per accepted socket, `IoUringEventLoop::on_accept` applies (Wowza's "Tune
Wowza Streaming Engine for optimal performance" maps onto these):

- `client_send_buffer_bytes` — `SO_SNDBUF`. Default 256 KiB. A pinned value
  bounds per-viewer kernel memory at high fan-out and makes a slow receiver
  visible to the application write queue (and so to the keyframe-aware
  slow-viewer policy) sooner. `0` restores kernel autosizing, which is the
  throughput-optimal choice on a link with headroom to spare.
- `client_receive_buffer_bytes` — `SO_RCVBUF`. Default `0` (autosize); the
  playback path receives almost nothing, so pinning it only helps a
  publisher socket on a lossy path.
- `client_tcp_notsent_lowat_bytes` — `TCP_NOTSENT_LOWAT`. Default 128 KiB.
  Caps the unsent bytes the kernel holds before the socket stops reporting
  writable, so pacing decisions run against a small queue. Must not exceed
  `client_send_buffer_bytes` when both are non-zero; ignored on kernels
  without the sockopt.

`ServerConfig::validate()` rejects a pinned buffer outside
`[2048, 67108864]` and a `notsent_lowat` above a pinned send buffer.

### Allocator

Set `MALLOC_ARENA_MAX` in the environment (the systemd unit and installer
do). glibc's default of 8 × CPU cores lets per-arena free lists retain
memory the process never returns to the OS, so RSS climbs under a
connection storm or many concurrent transcode threads and does not recede.
The installer writes `4 × cores` on a transcoding host (Wowza's
high-concurrency starting point) and `2` on a pure ingest/HLS origin.

## What one passthrough viewer costs

Passthrough is the cheap case on purpose: the publisher's H.264/AAC is
segmented once per stream and every viewer is served the same bytes. Nothing
in the delivery path is per-viewer work except the socket itself.

Per stream, once, regardless of audience:

* one segmenter, cutting on the publisher's own keyframes (no encode, no
  decode, no frame ever touched);
* one live window in memory, `hls_live_window_segments x
  hls_target_duration_seconds` of media;
* one cached object per segment and one per playlist generation in Varnish.

Per viewer:

* one socket at the TLS terminator, one at the cache;
* `2 / hls_target_duration_seconds` requests per second -- one media playlist
  poll and one segment fetch per segment;
* its share of the uplink, which is the publisher's bitrate.

Everything else is shared. The origin serves cache misses only, so its request
rate is set by the number of distinct objects (segments per second per stream),
not by the audience, and its bodies leave the process without a per-connection
copy -- the segment buffer is refcounted and written straight to the socket
(`AsyncHttpServer::Connection::out_body`).

### The one knob that changes the slope

Request rate is `2 / segment duration` per viewer, so segment length divides
the load on every hop at once -- TLS handshakes and request parsing at Caddy,
lookups at Varnish, packets and conntrack entries in the kernel:

| `hls_target_duration_seconds` | requests/s per viewer | at 50,000 viewers |
|---|---|---|
| 2 | 1.0 | 50,000 rps |
| 6 (default) | 0.33 | 16,700 rps |
| 10 | 0.2 | 10,000 rps |

The cost is latency: a live window is about three segments, so 10 s segments
put a viewer roughly 30 s behind the publisher. For rebroadcast and IPTV that
is invisible and the 40% request reduction is not. Set it with
`RTMP_HLS_TARGET_DURATION` at install time, or `hls_target_duration_seconds`
in `server.yaml`. The installer rewrites the cache's playlist TTL to half the
segment duration to match, so the cache keeps absorbing polls at the same
ratio.

Segment boundaries are keyframe boundaries and passthrough cannot insert
keyframes, so the publisher's GOP must divide the target. Raising the target
is always safe; lowering it below the encoder's keyframe interval is advisory
-- the segmenter still cuts on the next keyframe.

### Memory per viewer, at the cache

Varnish's `workspace_client` is allocated per session. At a large audience it
is multiplied by the concurrent viewer count, so a generous-looking value is
a direct subtraction from the memory available to cache segments -- and cache
memory is what keeps hit rate high, which is what keeps the origin idle. HLS
request headers are about a kilobyte; the 64k default is already ~60x
headroom.

## Host-level ceilings

Every limit below sits outside the server process. Each one presents the same
way from the outside -- throughput plateaus and new viewers stop joining while
existing ones keep streaming -- so they are easy to mistake for an application
limit. The installer now handles all four; they are documented here because a
hand-built or pre-existing host will not have them.

### Egress qdisc: never one root shaper

A single root HTB class (or a single CAKE instance) shapes through one qdisc
lock. Every outgoing packet on the NIC serialises through it, so total egress
is bounded by what one core can push through that lock -- measured at a few
Gbps on a link many times faster. At HLS bitrates that lands around four to
five thousand viewers, and it does not move when cores, RAM or link speed are
added, which is what makes it read as an application ceiling.

`rtmp-network-tune` therefore installs shaping per TX queue (`mq` root, one
HTB+`fq` per queue) so the work spreads across as many locks as there are
queues. CAKE remains for a single-queue NIC at or below 10 Gbps, where one
core can still drive the whole link. Above that, on a single queue, nothing is
shaped: plain `fq` keeps per-flow fairness with no global rate lock.

Shaping is also skipped whenever the configured rate is the installer's
virtual-NIC fallback rather than a measured or operator-supplied one. Shaping
to a guessed rate cannot protect a link whose real speed is unknown, and the
guess is high enough to select the worst branch.

### Softirq steering

A virtio NIC commonly exposes one hardware queue, so every packet's softirq
lands on one core and that core caps throughput while the rest idle. RPS fans
receive work across CPUs when the hardware cannot; RFS returns each flow to
the core running its socket. XPS is the transmit-side equivalent and is set
unconditionally -- a NIC with one queue per core has the most egress to
spread, and is exactly the case a receive-side condition would skip.

### Varnish threads

A `.ts` response is assembled whole (`do_stream=false`) and its thread is held
until the last byte reaches the client, so the thread requirement tracks
*concurrent viewers*, not request rate. Threads above `thread_pool_min` are
spawned at a throttled rate, which makes the pre-warmed floor -- not the
maximum -- what a join wave actually runs against. The installer sizes one
pool per core and a RAM-scaled floor up to 2500 threads per pool.

`varnishstat -1 | grep -E 'threads|sess_queued|sess_dropped'` shows this
directly: a rising `threads_limited` or `sess_queued` means the ramp, not the
cache, is the limit.

### Connection tracking

When a firewall loads `nf_conntrack`, its table is a hard per-flow ceiling
that the kernel sizes from RAM. A full table refuses new connections while
established ones continue undisturbed -- "nobody new can join past N" in its
purest form. The installer sizes the table from the connection budget and
shortens the timeouts that keep dead viewer flows occupying slots.

## Required production acceptance test

Build and start the production server on the target Linux host. From a
separate Linux generator host:

```bash
SERVER_HOST=10.0.0.10 \
PLAYBACK_NAME='concert' \
APPLICATION='live' \
VIEWERS=1000 \
DURATION=300 \
bash scripts/load-test.sh
```

The load generator exits non-zero unless every requested publisher and viewer
reaches streaming, no client fails, and every verified media payload is
uncorrupted.

Repeat at the intended bitrate and viewer count, then run a 24-hour soak.
Monitor:

- link utilization and packet rate;
- `process_memory_bytes`;
- `outbound_queue_bytes`;
- `dropped_video_frames` / `dropped_audio_frames`;
- `slow_viewer_evictions`;
- `io_uring_sq_full` / `io_uring_cq_overflow`;
- `provided_buffer_exhaustion`; and
- `partial_send_count`.

That command validates direct RTMP fan-out. Validate the separate public HLS
path through TLS, Caddy, and Varnish with the real copy/passthrough link:

```bash
URL=https://stream.example.com/hls/live/concert/master.m3u8 \
VIEWERS=30000 RAMP=10m HOLD=30m \
bash scripts/load-test-hls.sh
```

At this scale the generator side must be distributed unless one generator can
supply the full audience bitrate; otherwise it is the generator, not the VPS,
that the test measures.

Do not advertise a viewer capacity higher than the largest run that passes on
the actual server, NIC, kernel and network path. Direct RTMP capacity is
bounded first by:

```text
viewers <= usable outbound bits/second / total stream bits/second
```

The server does not transcode or reduce bitrate.
