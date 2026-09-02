# HLS packaging

Phase 6 adds HTTP-based viewer delivery alongside the existing RTMP egress
path. RTMP remains the ingest protocol; HLS is a packaging layer over the
same media the RTMP fan-out and the FLV recorder already see.

```text
OBS / RTMP encoder
      │
      ▼
RTMP ingest ──► CommandSession ──► TeeRecorderSink ──┬──► recording::Recorder ──► AsyncFileSink ──► .flv
                                                     │
                                                     └──► hls::Segmenter ──► hls::SegmentStore
                                                                                    │
                                                                control::HlsHttpHandler (HTTP/CDN)
```

## Scope

**Passthrough only.** H.264 video and AAC audio are repackaged from FLV/AVCC
into MPEG-TS without re-encoding. No encoder is built (explicitly out of
scope per `docs/v2_promot.md` Phase 6: "Do not build a raw H.264/AAC encoder
from scratch").

**Adaptive bitrate transcoding is deferred**, by design, to a separate
worker-process integration — see "Multiple renditions" below. Nothing in the
network worker path performs transcoding.

Non-AVC video or non-AAC audio is rejected at the parse step and counted in
`SegmenterStats::dropped_frames`; it is never half-packaged into a segment a
player cannot decode.

## Container choice: MPEG-TS, not CMAF/fMP4

Both were viable. MPEG-TS was chosen for this first implementation:

| Consideration | MPEG-TS | CMAF/fMP4 |
|---|---|---|
| Segment independence | Each segment carries its own PAT/PMT and is fully self-contained | Requires a separate init segment (`EXT-X-MAP`) whose lifetime must be coordinated with every media segment |
| Publisher reconnect | New parameters simply appear in the next segment's PMT | Init segment must be regenerated and re-associated; stale init breaks playback |
| Client support | Universal — every HLS client since v1 | Requires HLS v6+/fMP4-capable clients |
| Framing overhead | ~4% (188-byte packets, adaptation-field stuffing) | Lower |
| LL-HLS partial segments | Awkward | Natural fit |
| Shared segments with DASH | No | Yes |

The decisive factors were **segment independence** and **reconnect
robustness**: a live packager's hardest correctness problems are codec
changes and publisher reconnects, and TS handles both with strictly local
state. The ~4% overhead is an acceptable price for that, and a CDN absorbs
the bandwidth difference.

**Update:** low-latency HLS is now implemented, and on MPEG-TS rather than
CMAF. The "awkward" row above turned out to be a misjudgement: RFC 8216bis
only requires a partial segment to be an independently addressable resource,
not a CMAF chunk, and a TS part is exactly the segment's next run of whole TS
packets. Serving parts as their own URIs (rather than byte ranges of a
growing segment) also keeps every object immutable, which is what the shared
Varnish tier in front of this origin needs. Staying on TS means low latency
costs no init-segment coordination and no reconnect fragility.

A CMAF/fMP4 muxer does now exist (`media::mp4::Fmp4Muxer`) for the one thing
TS genuinely cannot do — sharing segments with MPEG-DASH — but the HLS
delivery path is unchanged and still all TS.

### Low-Latency HLS

Off by default (`hls_low_latency`). When on, the segmenter cuts each segment
into `hls_part_target_duration` slices and publishes each one the moment it is
closed, so a player runs roughly one part behind the encoder instead of
roughly three segments behind it.

| Piece | Where |
|---|---|
| Part cutting, independence marking | `hls::Segmenter`, `SegmenterConfig::part_target_duration` |
| Part storage, live edge, readiness | `hls::SegmentStore::add_part` / `live_edge` / `has_reached` |
| `EXT-X-PART`, `PART-INF`, `PRELOAD-HINT`, `PART-HOLD-BACK` | `hls::build_media_playlist` |
| Blocking reload (`_HLS_msn`, `_HLS_part`) | `control::HlsHttpHandler`, parked on `control::DeferredResponse` |

A part is named `segment-<sequence>.<index>.ts`, so its URL is as immutable
and as cacheable as a segment's. `INDEPENDENT=YES` is set only on a part that
actually opens with a keyframe — never assumed from its position, because a
segment opened by a forced mid-GOP cut has no keyframe at all.

**Blocking reload does not block a thread.** `AsyncHttpServer` runs handlers
on its event-loop threads, so waiting inside a handler would stall every other
connection that loop is carrying. Instead the handler returns a
`DeferredResponse`, the loop parks the connection (dropping its read interest
so a pipelined request cannot be answered out of order), and the media thread
resolves it from `SegmentStore`'s update notifier. Parked requests are bounded
per stream (`max_blocked_requests_per_stream`) and always answered — on
timeout with the playlist as it stands, on stream end, and on unregistration —
because a reset reads to a player as a media failure and costs a reconnect.

**Interaction with the cache tier.** A blocking request carries `_HLS_msn` in
its query, so it is a distinct cache key per live-edge position and several
players asking for the same position collapse onto one origin request. Those
parameters are stripped from the segment and part URIs inside the playlist
body: a media URL carrying a live-edge position would be a distinct cache
object per polling player for no benefit. Note that
`hls_blocking_reload_timeout` must stay below the cache tier's own backend
timeout, or Varnish gives up on the held request before the origin answers it.

### Segment encryption

Off by default (`hls_encryption_enabled`). AES-128-CBC over the whole segment
with PKCS#7 padding and `EXT-X-KEY`, which every HLS player implements.

Signed playback tokens stop an unauthorised player from *fetching* media, but
anything that obtains a segment URL gets plaintext. With encryption on, media
is useless without a key from `key-<id>.bin` under the same stream path, which
passes the same authorisation gate as the playlist and is served
`private, no-store`. `hls_key_rotation_interval` bounds how much media one
leaked key exposes; retired keys stay fetchable for a few rotations so a
player holding a slightly stale playlist is not stranded.

Encrypted media stays fully cacheable — every viewer receives the identical
ciphertext. Two consequences are deliberate:

* A partial segment is encrypted under its parent segment's key **and IV**
  (RFC 8216bis 6.2.3), as its own CBC stream, so it decrypts standalone from
  what `EXT-X-KEY` already advertises.
* An encrypted segment's `EXT-X-I-FRAMES-ONLY` byte range is dropped rather
  than kept: the range would point into ciphertext, and trick play that
  produces garbage is worse than trick play that is absent.

SAMPLE-AES is not offered. It needs codec-aware partial encryption and, in
practice, a DRM system's key delivery to be worth anything over this.

### Trick play

`iframe.m3u8` per stream, advertised from the master playlist as
`EXT-X-I-FRAME-STREAM-INF` for any rendition whose `iframe_uri` is set. The
segmenter records, as a side effect of packaging, the byte length of each
segment's leading run through its first keyframe — program tables included, so
the range is independently decodable — and the playlist emits it as
`EXT-X-BYTERANGE:<length>@0`. Granularity is therefore one I-frame per
segment; later keyframes inside a segment are not listed, because a range
starting at one would not carry its own PAT/PMT.

## Input format

RTMP delivers media message bodies already in FLV tag-body form:

- **Video** — `FrameType(4b)|CodecID(4b)`, `AVCPacketType(1)`,
  `CompositionTime(3, signed)`, then either an
  `AVCDecoderConfigurationRecord` (packet type 0) or a length-prefixed AVCC
  sample (packet type 1).
- **Audio** — `SoundFormat(4b)|Rate(2b)|Size(1b)|Type(1b)`,
  `AACPacketType(1)`, then either an `AudioSpecificConfig` (0) or a raw AAC
  frame (1).

`media::h264` and `media::aac` parse these. Every length field is validated
against the remaining buffer before use, so a malformed or hostile payload
produces an error rather than an over-read (`docs/v2_promot.md` 3.5).

## Conversion pipeline

1. **AVCC → Annex B** (`media::h264::avcc_to_annexb`). Length prefixes are
   replaced with 4-byte start codes. An Access Unit Delimiter is prepended.
   On keyframes the SPS/PPS from the decoder config are inserted in-band, so
   a player joining at *any* segment boundary can decode without having seen
   the original sequence header. In-band SPS/PPS/AUD NALs already present in
   the sample are dropped to avoid duplication.
2. **AAC → ADTS** (`media::aac::append_adts_header`). A 7-byte ADTS header
   (no CRC) is derived from the `AudioSpecificConfig`. The AAC payload bytes
   are untouched.
3. **MPEG-TS mux** (`media::ts::TsMuxer`). PAT (PID 0) and PMT (PID 0x1000)
   at the head of every segment; video on PID 0x100 as stream type 0x1B,
   audio on PID 0x101 as stream type 0x0F. PCR rides on the video PID.
   Keyframes set `random_access_indicator` and carry a PCR. Continuity
   counters advance per-PID across the whole stream, not per segment.
   Timestamps are RTMP milliseconds × 90 (the 90 kHz MPEG clock).

## Segmentation

`hls::Segmenter` cuts a new segment at the **first video keyframe at or after
`target_duration`** (default 4 s). Segments always start on a keyframe so
each is independently decodable.

Two bounds override that rule, because a client-influenced resource must
always have a limit (3.5):

- `max_segment_bytes` (default 16 MiB)
- `max_segment_duration` (default 20 s)

A publisher that never sends another keyframe therefore cannot grow the
segment buffer without limit. Such a **forced cut** is not keyframe-aligned,
so the resulting segment is marked discontinuous and counted in
`SegmenterStats::forced_cuts`.

Audio never opens a segment — audio arriving before the first keyframe is
dropped rather than written into a segment no player could start on.

## Discontinuity handling

`EXT-X-DISCONTINUITY` is emitted before a segment whenever the timeline does
not continue the previous one. Four triggers, all covered by tests:

| Trigger | Detection |
|---|---|
| Video codec header change | New SPS/PPS differ from the cached config |
| Audio codec header change | Object type, sample-rate index or channel config differ |
| Timestamp rollover / encoder reset | Backwards jump beyond `discontinuity_threshold` (default 5 s) |
| Stall / splice | Forwards jump beyond the same threshold |
| Publisher reconnect | Explicit `mark_publisher_reconnect()` |

Re-sending an *identical* sequence header is common encoder behaviour and is
correctly **not** treated as a change.

An internal `timeline_base_ms_` offset keeps output PTS/DTS monotonically
increasing across a rollover or reconnect, so the TS timeline never runs
backwards even though the RTMP timestamps do.

`EXT-X-DISCONTINUITY-SEQUENCE` is maintained by `SegmentStore`: discontinuities
that scroll out of the live window are counted, so a late-joining player
numbers its decoder resets correctly.

## Storage model

Segments live **in memory**, in `hls::SegmentStore`, not on disk.

Rationale: segments are short-lived, and serving them from RAM keeps disk I/O
entirely off the request path (and avoids the write-then-read amplification a
disk-backed packager pays). Segment bytes are held in a
`core::SharedBuffer` (`shared_ptr<const vector<byte>>`) — the same
immutable-shared-storage pattern `LiveFanout` uses for media frames (3.8), so
**a segment is never deep-copied per viewer** no matter how many players
fetch it concurrently.

Memory is bounded three ways:

- `live_window_segments` (default 6) — how many are advertised in the playlist
- `retention_grace_segments` (default 4) — kept but unadvertised, so a player
  holding a slightly stale playlist still gets a 200 rather than a 404
- `max_total_bytes` (default 256 MiB) — an absolute cap regardless of counts

Total retained is never more than window + grace. A segment evicted while a
viewer is mid-response stays valid for that viewer: the `shared_ptr` returned
by `find_segment()` keeps the bytes alive.

## HTTP delivery and CDN behaviour

Served by `control::HlsHttpHandler` through the **existing Phase 5
`control::HttpServer`** — chained in front of the management API handler, so
one bounded HTTP server serves both. Production wires Caddy's asynchronous
public HTTP/TLS transport to this loopback service. Segment responses retain
the store's `SharedBuffer` and write it separately from the HTTP headers, so
the C++ layer does not deep-copy a segment into a viewer-specific response.
No second C++ HTTP stack was introduced, and the Phase 5 posture (bounded
accept queue, bounded header/body sizes, fixed worker pool) is preserved.

Routes (GET/HEAD only):

```text
/hls/{application}/{stream}/master.m3u8   multivariant playlist
/hls/{application}/{stream}/index.m3u8    media playlist
/hls/{application}/{stream}/{name}.ts     one segment
```

The Linux production entry point creates one publisher-owned `hls::StreamSink`
through `RecorderFactory` after publish authorization. Its default production
target is 2 seconds with six advertised plus six grace segments and a 128 MiB
per-stream hard cap. Publisher disconnect finalizes the trailing segment and
marks the playlist ended; reconnect replaces the bounded store with a fresh
media sequence.

### Content types

| Resource | `Content-Type` |
|---|---|
| Playlists | `application/vnd.apple.mpegurl` |
| Segments | `video/mp2t` |

### Maximum-scale public mode

Production defaults `hls_high_scale_mode: true`. Caddy sends all `/hls/*`
traffic through the same-VPS Varnish instance. A fresh playlist open passes to
the origin once for a private opaque-session redirect. Redirected media
playlists are shared for 1 second and masters for 30 seconds; their bodies omit
viewer-specific query values, so request collapse cannot leak one player's
session to another. Varnish also request-collapses a hot segment's first fetch
and caches the complete immutable segment for 1 hour. Segment sequence numbers
start from wall-clock milliseconds, so names cannot repeat after publisher
reconnect or process restart while an older cache object still exists.

`viewer-estimator.service` consumes Varnish's shared-memory access log (it does
not persist one access-log line per request). It counts sessions from recurring
media-playlist URLs over a 20-second window, and attributes query-free segment
bytes by their concrete stream/rendition path. Because Varnish observes both
HIT and MISS responses, the admin homepage includes cache-served viewers and
traffic rather than reporting only origin misses. The origin's own
delivery-stats mutex remains disabled in this mode.

### Where a link's viewer count comes from

The origin **cannot** count HLS viewers. A media playlist is cached for one
second, so a thousand players polling one link produce roughly one origin
request per second, and `HlsHttpHandler::link_stats` therefore reports about
one viewer no matter how large the audience really is. Only the edge sees
every request.

The server reads the estimator's file itself (`control::EdgeViewerStats`,
path from `edge_viewer_stats_path`, re-read at most once a second) so the
real per-link number is part of what the API reports, rather than something
each client has to go and assemble for itself:

| Link | Field | Source |
|---|---|---|
| Published stream | `viewer_count` | `rtmp_viewer_count` + `hls_viewer_count` |
| Published stream | `hls_viewer_count` | edge key `app/stream` |
| Source-transcode job | `viewer_count` | edge keys for the job name and every rendition output |
| One rendition of a job | `outputs[].viewer_count` | edge key `app/<rendition>` |
| Whole server | `hls_active_viewers` gauge | the estimator's session-union total |

`hls_viewers_measured` (streams) and `delivery_stats_available` (jobs) say
whether the number is an edge measurement. When they are false the edge file
was missing or older than its own window, and the figure falls back to what
the origin itself observed — a floor, not a count. The admin panel keeps its
own reading of the same file only as a fallback for a server that predates
this, and does not add it on top of a server-measured count.

Per-link counts are summed across a job's renditions, so a player that is
mid-ABR-switch (briefly visible under two rendition keys inside the 20s
window) can count twice on that job's total. The server-wide
`hls_active_viewers` gauge uses the estimator's session union and never
double-counts.

This profile is deliberately for public links. Query-token authorization still
requires a private-cache policy; the installer intentionally shares immutable
segment objects across every viewer.

After the one-time session redirect, the cache eliminates proportional
playlist generation and segment-origin body work, but not network egress.
One VPS still sends one copy to every viewer. Approximate ceiling:

```text
viewers = usable outbound Mbps / average selected-rendition Mbps
```

Keep 10-20% headroom for protocol overhead, bursts and retransmissions and use
the provider's committed egress rate, not merely the virtual NIC's displayed
link speed.

The production installer enforces that headroom with fair egress scheduling.
On a multi-queue NIC it shapes under `mq`, splitting the target rate evenly
across an HTB class plus `fq` per TX queue (`scripts/install-linux.sh`,
`rtmp-network-tune`) so shaping work spreads across as many cores as there
are queues instead of serialising every outgoing packet through one root
qdisc's lock — the single-queue path (CAKE up to 10 Gbps, or HTB plus `fq`
above it) is used only when the NIC exposes one TX queue and there is no
parallelism to gain. Consequently a new viewer's playlist and first
advertised (already cached) segment are not stuck behind all established
viewers in the provider's saturated FIFO/policer queue, and total shaping
throughput is not capped by a single CPU core's packet-processing rate.

### Playlist reload cadence

Every media playlist carries `#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=NO`.
The origin does not implement blocking playlist reload, and the tag tells a
compliant player to re-fetch at the `EXT-X-TARGETDURATION` cadence rather
than polling faster — the HLS analogue of Wowza's "client idle frequency",
and the main lever on per-viewer origin request rate. Combined with the
6 s segment default (`SegmentStoreConfig::target_duration_seconds`) and the
`public, max-age=1` micro-cache below, a large audience collapses to roughly
one origin playlist fetch per second per link.

`SegmentStoreConfig::playlist_hold_back_seconds` (0 = player default of
3 × TARGETDURATION) sets the advertised `HOLD-BACK`; a value under the
3 × TARGETDURATION floor is clamped up per RFC 8216bis.

### Cache-Control

| Resource | Header | Why |
|---|---|---|
| Media playlist (library default) | `no-cache, max-age=0` | Safe when no reverse cache profile is selected |
| Initial playlist redirect (production high-scale) | `private, no-store` | Mints a distinct playback session without caching the redirect |
| Media playlist (production high-scale) | `public, max-age=1, s-maxage=1` | Collapses synchronized polls; body contains no viewer state |
| Segment (200) | `public, max-age=31536000, immutable` | Uniquely named and never rewritten — safe to cache indefinitely |
| Master playlist (production high-scale) | `public, max-age=30, s-maxage=30` after the private redirect | Rendition set changes rarely; body contains no viewer state |
| Segment (404) | `no-store` | An evicted-segment 404 must not be cached, or the CDN keeps serving it after recovery |
| 403 (bad token) | `no-store` | An authorization result must never be cached across viewers |

### Range requests

Single-range `Range: bytes=...` is supported on segments (`Accept-Ranges:
bytes`), including open-ended (`bytes=1000-`) and suffix (`bytes=-50`) forms,
returning `206` with `Content-Range`. Unsatisfiable or malformed ranges
return `416` with `Content-Range: bytes */<size>`. Multi-range is not
supported (no real player or CDN requires it).

### CORS

`Access-Control-Allow-Origin: *` plus
`Access-Control-Expose-Headers: Content-Length,Content-Range`, required for
browser players (hls.js) fetching cross-origin. Configurable; empty disables.

## Playback authorization

When `require_playback_token` is set, every playlist and segment request must
carry `?token=...&expires=...`, verified with **the same
`management::verify_token`** the RTMP `play` path uses — identical claims
(application, stream name, expiry), identical secret, constant-time
comparison, no database lookup on the request path. A token therefore works
for both RTMP and HLS playback, or for neither.

The media playlist propagates the caller's query string onto each segment URI
so a token-gated stream stays playable: the player copies the URI verbatim.
Tag lines are never decorated.

## Multiple renditions

`build_master_playlist` emits `EXT-X-STREAM-INF` variants ordered
lowest-bandwidth-first (so a player without measured throughput starts
conservatively). `CODECS` is derived from the SPS profile/compat/level and
the AAC object type per RFC 6381 (`Segmenter::codecs_attribute()`).

With passthrough alone there is exactly one rendition — the ingest quality.
Adaptive variants require transcoding, which is deliberately **not** in this
phase and must run as a **separate worker process**: the RTMP ingest process
would hand it decoded frames or a stream reference over IPC, and the worker
would register additional `SegmentStore`s and renditions through the same
`HlsHttpHandler` API used here. Nothing about the current design has to
change to accommodate that — which is why the rendition list is already a
first-class, per-stream, settable collection rather than a hardcoded single
entry.

## Threading

- `Segmenter` runs on the RTMP media thread. It performs only CPU-bound
  buffer manipulation — no syscalls, no disk, no locks — so it is safe there.
- `SegmentStore` is the thread boundary: a short mutex, no I/O and no
  callbacks under the lock (3.7). Playlist rendering happens outside the lock.
- `HlsHttpHandler` runs only on `HttpServer` worker threads and performs no
  disk I/O at all.

## Scaling past one box

The co-located Varnish makes one box serve a large audience; its ceiling is
uplink bandwidth. To add total bandwidth, regions, or machine-failure
resilience, put **edge** cache nodes in front of the origin — see
[multi-node-hls.md](multi-node-hls.md). The origin's `hls_edge_fetch_secret`
gates `/hls` to token-bearing edge requests only; `deploy/edge/install-edge.sh`
plus `deploy/varnish/streamforge-edge.vcl` stand up an edge or an
origin-shield node.

## Known limitations

- Segments are in-memory only, so they do not survive a process restart —
  a restarting server starts a fresh live window. This is correct for live
  but means HLS cannot serve VOD of a past stream; use the FLV recording for
  that.
- Single-range Range requests only.
- MPEG-DASH delivery now exists as a separate module (`docs/dash.md`),
  not folded into this document because it is a distinct delivery surface
  with its own container (fMP4/CMAF) and manifest format (MPD) — HLS itself
  is unaffected and stays all-TS.
- No WebRTC (WHIP/WHEP) or SRT ingest; RTMP is the only publish protocol.
- Multi-node edges are stood up by script (`deploy/edge/install-edge.sh`),
  not by a control plane that discovers or rebalances them.
- SAMPLE-AES and DRM key systems (Widevine/FairPlay/PlayReady) are out of
  scope; `EXT-X-KEY` here is AES-128 with the identity key format.
