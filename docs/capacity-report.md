# Capacity report

Phase 7 (`docs/v2_promot.md` PHASE 7 "Capacity report").

> **Read this section first.** Every number below was produced by running
> `apps/rtmp_load_gen` — real TCP sockets, real RTMP handshakes, real AMF0
> commands, real H.264/AAC-shaped payloads — against a **test-only,
> poll(2)-based** server front-end, *not* against the production io_uring
> transport. The production transport (`apps/rtmp_server`,
> `src/io/io_uring/*`) requires Linux io_uring and **cannot be built or run on
> the host these measurements were taken on**. Section 6 lists exactly which
> required scenarios are therefore **not measurable here**, and gives the
> commands to obtain them on a Linux host.
>
> Nothing in this document is modelled, extrapolated, or estimated. Rows that
> could not be measured say so.

---

## 1. Test environment

| Item | Value |
|---|---|
| Hardware | Apple MacBook Pro, Apple M1 (`machdep.cpu.brand_string`) |
| CPU | Apple M1, 8 logical cores (4 performance + 4 efficiency) |
| RAM | 8 GiB (8,589,934,592 bytes) |
| NIC | **None used — loopback (`lo0`, 127.0.0.1) only.** No physical NIC was in the path. |
| Kernel | Darwin 25.3.0, `xnu-12377.91.3~2/RELEASE_ARM64_T8103`, macOS 26.3.1 (25D771280a) |
| Compiler | Apple clang 21.0.0 (`clang-2100.1.1.101`), target `arm64-apple-darwin25.3.0` |
| CMake / generator | CMake 4.4.0, Ninja |
| Build preset | `core-only` (`RTMP_SERVER_CORE_ONLY=ON`) |
| Build type / flags | **`Debug`, `-g`, no optimisation (`-O0`)** |
| File descriptor limit | `ulimit -n 8192` (set by the harness) |
| Server under test | `apps/rtmp_test_server` — **test-only**, single-threaded poll(2) loop |
| Load generator | `apps/rtmp_load_gen`, single-threaded poll(2) loop, same host |

### 1.1 Four reasons these numbers are a lower bound on production

1. **Debug, unoptimised build.** `-O0 -g`. A `release` build is materially
   faster; no `-O2` measurement was taken.
2. **No io_uring.** The production path uses registered buffer rings,
   multishot accept/recv, and batched submission. The harness uses one
   `poll()` + `recv()` + `send()` per connection per tick.
3. **Single-threaded server, single core.** No `SO_REUSEPORT` sharding, no
   `CrossWorkerRouter`, no multi-worker scaling. The production server runs
   one event loop per worker.
4. **Generator and server share one 8-core laptop.** They compete for the
   same CPU, and loopback traffic is copied twice. At the higher viewer
   counts the *generator* is a proven bottleneck (§4.2).

---

## 2. Workload

Unless a scenario says otherwise:

| Parameter | Value |
|---|---|
| Video | 2,500,000 bps, 30 fps, H.264/AVC FLV tags, keyframe every 60 frames (2 s GOP) |
| Audio | 128,000 bps AAC, 44.1 kHz stereo, 1024-sample frames |
| Source bitrate per publisher | ~2.63 Mbps payload (measured ingress 2.79–2.90 Mbps including RTMP chunk overhead) |
| Keyframe size | ~8× an inter frame, GOP bit budget preserved |
| Payload verification | every received payload byte-checked (`MediaSource::verify_pattern`) |

Reproduce with:

```bash
cmake --preset core-only -DRTMP_SERVER_CORE_ONLY=ON
cmake --build --preset core-only
./scripts/phase7_load_matrix.sh          # all nine scenarios
```

---

## 3. Measured results

All rows measured on the environment in §1. "Viewers reached play" is the
number that received a `NetStream.Play.Start` acknowledgement **and** media,
not the number requested.

| # | Scenario | Viewers requested → reached play | Ingress | Egress (measured at viewers) | Peak RSS | Video/audio drops | Evictions | Corrupt payloads | Duration |
|---|---|---|---|---|---|---|---|---|---|
| 01 | 1 pub + 100 viewers | 100 → **100** | 2.80 Mbps | **238 Mbps** | 21 MB | 3,701 / 5,397 | 0 | **0 / 131,638** | 20 s |
| 02 | 1 pub + 500 viewers | 500 → **161** | 3.28 Mbps | 126 Mbps | 167 MB | 5,141 / 7,492 | 0 | **0 / 68,087** | 20 s |
| 03 | 1 pub + 1000 viewers | 1000 → **160** | 3.76 Mbps | 150 Mbps | 209 MB | 4,422 / 6,477 | 0 | **0 / 82,351** | 20 s |
| 04 | 10 pubs + 100 viewers each | 1000 → **311** (4/10 pubs) | 8.97 Mbps | 142 Mbps | 174 MB | 4,826 / 7,039 | 0 | **0 / 77,630** | 20 s |
| 05 | Viewer burst (500, ramp-up 0) | 500 → **273** | 2.00 Mbps | 29 Mbps | 51 MB | 0 / 0 | 0 | **0 / 13,674** | 20 s |
| 06 | Slow viewers (200, 50 % @ 2 KB/tick) | 200 → **200** | 2.86 Mbps | 184 Mbps | 295 MB | 3,464 / 5,047 | 0 | **0 / 127,050** | 25 s |
| 07 | Publisher reconnect (every 5 s) | 100 → **100**, 4 reconnects | 8.10 Mbps | 46 Mbps | 5 MB | 0 / 0 | 0 | **0 / 31,461** | 25 s |
| 08 | Abrupt disconnect (50 % RST) | 200 → **200**, 100 RST | 2.90 Mbps | 220 Mbps | 196 MB | 2,376 / 3,465 | 0 | **0 / 125,650** | 21 s |
| 09 | Large keyframes (8 Mbps, 10 s GOP) | 100 → **66** | 7.86 Mbps | 194 Mbps | **473 MB** | 5,133 / 7,447 | **27** | **0 / 38,248** | 22 s |

Peak observed egress across all samples: **434 Mbps** (scenario 06).

### 3.1 What passed unambiguously

* **Zero corrupt payloads in every scenario** — 695,778 media payloads
  byte-verified end-to-end across the matrix. The chunk encoder, chunk
  decoder, GOP cache and fan-out preserve media exactly.
* **Abrupt disconnect (08).** 100 viewers RST mid-run with no RTMP teardown.
  The server went from 201 connections / 165 viewers to 101 / 100 and the
  publisher kept streaming. Connection and subscriber cleanup is correct
  under RST.
* **Publisher reconnect (07).** 4 full reconnect cycles (abrupt drop → new
  TCP → handshake → connect → createStream → publish). No crash, no leak
  (RSS stayed ~5 MB), viewer connections survived.
* **Slow viewers (06).** All 200 viewers, half reading only 2 KB/tick,
  reached playback; the staged policy dropped frames and recorded 100
  recoveries without killing healthy viewers.
* **Large keyframes (09).** 10-second GOPs at 8 Mbps drove `gop_cache_bytes`
  to 10.1 MB and produced the matrix's only **evictions (27)** — the
  slow-viewer policy genuinely fires under keyframe pressure.

### 3.2 Metric plumbing validated by real traffic

Every metric marked "core-only ✅" in `docs/observability.md` §2.4 moved in
response to real socket traffic: `active_connections` (tracked 27→1000),
`active_publishers` (1–4), `active_viewers`, `viewers_per_stream_max`,
`ingress_bitrate`, `egress_bitrate`, `gop_cache_bytes` (up to 10.1 MB),
`outbound_queue_bytes`, `dropped_video_frames`, `dropped_audio_frames`,
`slow_viewer_recoveries`, `slow_viewer_evictions`, `partial_send_count`
(up to 451), `process_memory_bytes`.

---

## 4. Capacity analysis

### 4.1 Protocol capacity

Measured and sound. The protocol layer handled 695,778 verified media
payloads with **zero corruption**, across chunk sizes up to 8 Mbps keyframes,
publisher reconnects, and RST disconnects. `first media latency` was
**4–5 µs p50** in every scenario: once a viewer is subscribed, the GOP cache
serves it essentially instantly.

Connection *setup* is the protocol-level constraint on this harness:
handshake p50 rose from 12 ms (100 viewers) to 1.9 s (1000 viewers), because
one thread performs every handshake serially.

**Protocol capacity conclusion:** correctness is not the limit. No protocol
error, malformed message, or corruption occurred at any tested scale.

### 4.2 CPU capacity

**This is the binding constraint on this host, and it is the *harness*, not
the server design.**

Evidence that the ceiling is the single-threaded pair, not the server:

* Scenarios 02 and 03 requested 500 and 1000 viewers and both plateaued at
  ~160 — the same number. A server-side limit would scale with offered load;
  an identical plateau at two different offered loads is a fixed-rate
  bottleneck.
* TCP connect p99 reached 4.9 s (02) and 7.4 s (03) — connections sat in the
  accept backlog because one thread could not accept, handshake and fan out
  concurrently.
* Aggregate egress saturated at **350–434 Mbps** regardless of viewer count.

> **Therefore: this report does NOT claim the server supports 1,000 viewers.**
> The honest measured figure on this host is **~160–311 concurrent RTMP
> viewers** sustained on a single-threaded, `-O0`, non-io_uring loop.
> `docs/v2_promot.md` explicitly warns against claiming a viewer count that a
> benchmark did not actually demonstrate; scenarios 02, 03 and 04 did not
> demonstrate theirs, and are reported as **partial**.

Per-core scaling could not be measured: the test server is single-threaded by
construction and `worker_cpu_usage` is not yet instrumented
(`docs/observability.md` §2.5).

### 4.3 Memory capacity

Measured via `process_memory_bytes` (RSS).

| Load | Peak RSS | Per-viewer marginal |
|---|---|---|
| 100 viewers, 2.5 Mbps | 21 MB | — |
| 160–311 viewers | 167–209 MB | ~0.6–0.7 MB/viewer |
| 200 viewers, 50 % slow | 295 MB | ~1.4 MB/viewer |
| 66 viewers, 8 Mbps / 10 s GOP | **473 MB** | ~7 MB/viewer |

Memory is dominated by **per-viewer outbound queues**, not by the GOP cache
(which stayed ≤10.1 MB and respected its 16 MB budget in every scenario).
Slow viewers and long GOPs are the two multipliers.

Bounds held: the GOP cache never exceeded its configured limit, and RSS
always fell back after load (473 MB → 34 MB in scenario 09) — no leak.

**Caveat:** `outbound_queue_bytes` reached 453 MB in scenario 04 while RSS
was 174 MB. The gauge over-reports because `LiveFanout` never calls
`ViewerQueue::note_flushed()` — see §5.1. Treat the RSS column as
authoritative and the queue gauge as directional until that is fixed.

### 4.4 Network bandwidth capacity

**Not measured.** All traffic was loopback; no physical NIC was in the path,
so no NIC-limited number exists in this report.

What loopback *does* establish is fan-out amplification: one 2.8 Mbps
publisher produced 238 Mbps of egress to 100 viewers — **85×**, matching the
viewer count, confirming media is fanned out without per-viewer re-encoding.

On real hardware, egress bandwidth will bind before CPU for most deployments:

```
viewers_supported ≈ (NIC line rate × utilisation target) / source bitrate
```

At 2.63 Mbps source and 70 % of a 10 GbE link: ≈ 2,660 viewers per NIC.
**This is arithmetic, not a measurement**, and must be validated on the target
host before it is relied upon.

---

## 5. Defects surfaced by this load testing

### 5.1 `LiveFanout` never calls `ViewerQueue::note_flushed()` — spurious drops on healthy viewers

`ViewerQueue`'s contract (`viewer_queue.hpp:101-110`) states that a
transport-less caller such as `LiveFanout` must call `note_flushed()`
immediately after `offer()` returns `Deliver`/`DeliverResumed`. It does not.
`bytes_` therefore accumulates monotonically for the lifetime of a
subscription until it crosses `max_bytes` (8 MB default), trips
`WaitingForKeyframe`, resets to zero, and repeats — a sawtooth.

**Measured consequence:** scenario 01 has 100 healthy viewers on loopback with
no real backpressure, yet recorded **3,701 dropped video frames, 5,397 dropped
audio frames and 100 slow-viewer recoveries**. `outbound_queue_bytes`
oscillating between 92 MB and 306 MB is the same artefact.

Not fixed in Phase 7: the correct fix is not to call `note_flushed()` (that
would disable `LiveFanout`'s byte budget entirely, since it passes
`external_pending_bytes = 0`), but to feed the real transport backlog into
`offer()`. That is a Phase 3 fan-out design change and needs its own
before/after measurements. **Recommended as the first item of the next phase.**

### 5.2 Viewers are not re-attached after a publisher reconnect

Scenario 07: after each reconnect, `egress_bitrate` fell to 0 and
`gop_cache_bytes` / `outbound_queue_bytes` froze, while all 100 viewer TCP
connections stayed open. Viewers received 31,461 media messages over 25 s
versus 131,638 over 20 s in scenario 01.

`LiveFanout::publisher_stopped()` notifies subscribers and drops the stream
state; the republish mints a fresh `StreamId`, and the old subscribers are not
migrated. This may be intended RTMP semantics (a viewer should observe
`NetStream.Play.UnpublishNotify` and re-issue `play`), but the server
currently sends no such notification, so a real player has nothing to react
to. Needs a product decision plus either an `onStatus` notification or
automatic re-attachment.

### 5.3 Partial sends discarded the unsent remainder (fixed)

`TcpConnection::on_send_completion()` ignored `bytes_sent` and popped the
whole front buffer on any success, silently dropping whatever the kernel did
not accept. Fixed in Phase 7 (`front_offset_` + resume-from-offset). **Not
compile-verified on this host** — that file is Linux-only. See
`docs/phase-7-report.md`.

---

## 6. NOT MEASURABLE ON THIS HOST — requires a Linux / io_uring host

The following are **required by Phase 7 and are deliberately left unmeasured
rather than estimated**, because they characterise the production io_uring
transport, which cannot be built here (`CMakeLists.txt` guards
`src/io/io_uring` and `apps/rtmp_server` behind
`NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux"`).

| Required item | Status | Why |
|---|---|---|
| 1 pub + 1,000 viewers against the production transport | **not measurable on this host** | needs io_uring + multi-worker; the poll harness plateaus at ~160 |
| 10 publishers + 100 viewers each, all reaching play | **not measurable on this host** | only 4/10 publishers and 311/1000 viewers established here |
| 24-hour soak test | **not run** | requires a dedicated Linux host; not attempted, not simulated |
| Real CPU utilisation / `worker_cpu_usage` | **not measurable** | metric not yet instrumented (`docs/observability.md` §2.5) |
| Per-worker scaling, `connections_per_worker` | **not measurable** | single-threaded harness has exactly one worker |
| `io_uring_sq_full`, `io_uring_cq_overflow`, `provided_buffer_exhaustion` | **not measurable** | io_uring-specific; plumbing added, never executed |
| `inter_worker_queue_depth` / `_drops` | **not measurable** | `CrossWorkerRouter` is Linux-only *and* not yet instrumented |
| Real NIC throughput / NIC-bound capacity | **not measurable** | loopback only; no physical NIC in path |
| Release (`-O2`) performance | **not measured** | all figures are `-O0 -g` |
| Recording / HLS under load | **not measured** | not enabled in the test server |

### 6.1 Commands to obtain these on a Linux host

```bash
# 1. Build the production server with the io_uring transport.
cmake --preset release                 # RTMP_SERVER_CORE_ONLY stays OFF
cmake --build --preset release
ctest --preset release

# 2. Raise limits (1,000 viewers needs descriptors and backlog headroom).
ulimit -n 65536
sudo sysctl -w net.core.somaxconn=4096
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=8192
sudo sysctl -w net.core.rmem_max=16777216
sudo sysctl -w net.core.wmem_max=16777216

# 3. Start the real server (multi-worker, SO_REUSEPORT).
./build/release/apps/rtmp_server/rtmp_server --config config/server.toml &

# 4. Run the same scenario matrix against the real transport.
SERVER_BIN=./build/release/apps/rtmp_server/rtmp_server \
SERVER_ARGS="--config config/server.toml" \
BUILD_DIR=./build/release \
./scripts/phase7_load_matrix.sh

# 5. Drive the load generator from a SEPARATE machine over a real NIC, so
#    generator CPU and loopback copying are removed from the measurement.
./build/release/apps/rtmp_load_gen/rtmp_load_gen \
    --host <server-ip> --port 1935 \
    --publishers 1 --viewers 1000 --duration 300 --ramp-up 30000 \
    --video-bitrate 2500000 --fps 30 --keyframe-interval 60

# 6. 24-hour soak.
./build/release/apps/rtmp_load_gen/rtmp_load_gen \
    --host <server-ip> --publishers 5 --viewers 200 \
    --duration 86400 --ramp-up 60000 \
    --publisher-reconnect 3600 --slow-viewers 0.05 --abrupt-disconnects 0.05

# 7. Scrape metrics throughout; watch process_memory_bytes for leak drift.
watch -n5 'curl -s localhost:9000/metrics | grep -E \
  "active_(connections|viewers|publishers)|_bitrate|dropped_|slow_viewer_|\
io_uring_|process_memory|partial_send"'
```

Acceptance criteria for that run: `payloads_corrupt == 0`,
`viewers_streaming == requested`, `slow_viewer_evictions` attributable only to
deliberately-slow clients, and `process_memory_bytes` flat over 24 h.

---

## 7. Safe recommended capacity

### 7.1 Backed by measurement on this host

For a **single-threaded, `-O0`, poll-based** deployment on Apple M1 loopback —
the only configuration actually measured:

| Metric | Safe recommendation |
|---|---|
| Concurrent RTMP viewers per worker | **≤ 150** at 2.5 Mbps (measured stable at 100; 160 was the saturation plateau) |
| Concurrent viewers, 8 Mbps / 10 s GOP | **≤ 50** (66 requested → evictions began at ~39 active) |
| Aggregate egress per worker | **≤ 250 Mbps** (peak observed 434 Mbps, with degradation) |
| Publishers per worker | **≤ 4** at 2.5 Mbps (10 requested → 4 established) |
| RSS budget | **≥ 1.5 MB per viewer** at 2.5 Mbps; **≥ 7 MB per viewer** at 8 Mbps / 10 s GOP |

### 7.2 NOT established

**No production capacity recommendation can be made from this report.** The
production io_uring transport, multi-worker scaling, release optimisation and
real NIC behaviour were all outside what this host can execute. Section 6.1
must be run on a Linux host before any capacity commitment is made to an
operator.

Two prerequisites before that run is meaningful:

1. Fix §5.1 (spurious drops), or drop counts on the real transport will be
   uninterpretable.
2. Instrument `worker_cpu_usage` and `connections_per_worker`, or per-core
   scaling cannot be attributed.
