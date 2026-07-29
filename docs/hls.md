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

**Consequence:** low-latency HLS (LL-HLS partial segments, blocking playlist
reload, preload hints) is *not* implemented in this phase. Moving to CMAF is
the natural prerequisite for it, and is recorded as the recommended follow-up
in `docs/phase-6-report.md`.

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

### Cache-Control

| Resource | Header | Why |
|---|---|---|
| Media playlist | `no-cache, max-age=0` | Changes every segment duration; a cached live playlist stalls viewers |
| Segment (200) | `public, max-age=31536000, immutable` | Uniquely named and never rewritten — safe to cache indefinitely |
| Master playlist | `public, max-age=60` | Changes only when the rendition set changes |
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

## Known limitations

- No LL-HLS (partial segments, blocking playlist reload). Requires CMAF.
- No `EXT-X-KEY` / AES-128 segment encryption; access control is via signed
  URL tokens only.
- No I-frame-only (trick play) playlists.
- Segments are in-memory only, so they do not survive a process restart —
  a restarting server starts a fresh live window. This is correct for live
  but means HLS cannot serve VOD of a past stream; use the FLV recording for
  that.
- Single-range Range requests only.
