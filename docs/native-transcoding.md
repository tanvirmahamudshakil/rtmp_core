# Native CPU H.265 (HEVC) transcoding

This is an **in-process, FFmpeg-free** transcoding pipeline that produces HEVC
renditions on the CPU with a professional-grade quality-per-bitrate
configuration. The old FFmpeg-process-spawning transcoder has been removed;
this native pipeline is the only transcoding path.

```text
FLV/AVCC H.264 sample
    |
avcc_to_annexb (media::h264)          # length-prefixed -> Annex B, SPS/PPS on IDR
    |
H264Decoder  (openh264)              # -> I420 frame
    |
Scaler       (libyuv)                # box-filtered scale + centre-crop / letterbox
    |
HevcEncoder  (x265)                  # -> HEVC Annex B access units (VPS/SPS/PPS on IDR)
    |
TsMuxer / segment store              # HEVC PES on a 90 kHz clock
```

Everything runs software-only: **openh264** decodes H.264, **libde265** decodes
HEVC (for HEVC sources), **libyuv** scales I420, **x265** encodes HEVC. No GPU,
no FFmpeg process.

> **Licensing note:** libde265 is LGPL-2.1, unlike the BSD-family libraries
> above. The CMake wiring links it via `PkgConfig::LIBDE265` (the system's
> shared library through pkg-config) to stay LGPL-compliant — do not switch
> this to a static link.

Audio is transcoded in parallel with **libfdk-aac** — the highest-quality open
AAC implementation:

```text
FLV/AAC raw frame
    |
AacDecoder   (libfdk-aac)             # AudioSpecificConfig-configured -> S16 PCM
    |
AacEncoder   (libfdk-aac)             # AAC-LC / HE-AAC / HE-AACv2, ADTS-framed
    |
TsMuxer audio PES
```

Re-encoding (rather than passing the source AAC through) lets each rendition
carry its own audio bitrate/profile — e.g. a mobile rung at 48 kbit HE-AACv2 —
while the source stays high-rate. No resampling is done: the encoder follows the
source sample rate, so decode and encode frame sizes line up without a rate
converter. Where a rendition keeps the source audio format unchanged, the
existing ADTS passthrough path is still the cheaper choice.

## Why libfdk-aac for audio

libfdk-aac beats the generic encoders most at low bitrates, which is exactly
where a rendition ladder lives:

- **AAC-LC** (AOT 2) — universal, the default above 64 kbit.
- **HE-AAC** (AOT 5, SBR) and **HE-AACv2** (AOT 29, SBR + parametric stereo) —
  hold quality far below 64 kbit for mobile rungs. `build_aac_param_set`
  auto-selects them below a configurable threshold (stereo → HE-AACv2, mono →
  HE-AAC), and downgrades HE-AACv2 → HE-AAC for non-stereo sources.
- **Afterburner** — libfdk's extra bit-distribution search, on by default for a
  measurable quality gain at the same bitrate.

Output PTS is derived from the running output sample count (anchored to the
first frame), so it stays exact across the encoder's priming delay instead of
copying possibly-jittery input timestamps.

## Why HEVC gives the same quality at a lower bitrate

The saving does not come from the codec alone — it comes from how the encoder
is driven. `native/hevc_params.cpp` maps each preset onto x265 like this:

- **CRF + VBV rate control** (default). A Constant Rate Factor anchors the
  *perceived* quality; the preset's `video_bitrate` becomes the VBV **ceiling**
  (`vbv-maxrate`) rather than a fixed target. Easy scenes therefore spend far
  fewer bits than a fixed-ABR ladder would, while hard scenes stay under the
  advertised cap. Set `constrain_to_bitrate = false` for strict ABR instead.
- **Adaptive quantisation** (`aq-mode 3`, auto-variance with dark/edge bias) —
  keeps detail in shadows and flat gradients, where banding is most visible.
- **Psychovisual RD** (`psy-rd 2.0`, `psy-rdoq 1.0`) — preserves apparent
  detail and texture energy the eye notices even when it is not strictly
  rate-optimal. After AQ this is the single biggest "looks the same, costs
  less" lever.
- **Motion / reference depth** (`bframes 4`, `ref 4`, `b-adapt 2`, weighted
  P/B) — more compression per bit at the cost of CPU.
- **Fixed GOP** (`keyint == min-keyint`, `scenecut 0`) so keyframes land on a
  predictable cadence and HLS/TS segments align to them, with `repeat-headers`
  so every IDR (and therefore every segment) is independently decodable.

The knobs live in `HevcQualityOptions`; the defaults target live streaming at
`preset = "medium"`, `tune = "zerolatency"`, `crf = 23`.

## Building

On by default (`RTMP_ENABLE_NATIVE_TRANSCODE=ON`), but only actually compiled
when the codec libraries are present — the CMake probe degrades gracefully to a
build without transcoding when they are not. Install the dev packages first:

```bash
sudo apt-get install libx265-dev libx264-dev libopenh264-dev libde265-dev libfdk-aac-dev libcurl4-openssl-dev libyuv-dev
cmake -S . -B build -DRTMP_ENABLE_NATIVE_TRANSCODE=ON
cmake --build build
```

## Source-transcode jobs (pull an external URL)

Beyond transcoding streams published to this origin, the native pipeline can
pull an **external source URL**, transcode it per a template, and re-serve it as
one adaptive master `.m3u8`:

```text
source URL (rtmp://, HTTP(S) HLS, or raw HTTP TS; H.264/AAC)
    → native RtmpSourceClient, or HttpClient + content-based HLS/TS detection
    → FLV AVC/AAC conversion, or TsDemuxer
    → decode once (openh264 / libfdk-aac)
    → per rendition: libyuv scale + libx264 H.264 encode + AAC re-encode
    → RenditionFeed → Segmenter → SegmentStore
    → /hls/{app}/{name}/master.m3u8
```

Output is **H.264 + AAC** so the existing HLS packaging serves it unchanged.
Decode stays on openh264; encode is **libx264** (CRF + VBV-capped, zero
B-frames, `veryfast`/`zerolatency`) for CABAC and materially better
quality-per-bitrate than openh264's CAVLC-only encoder at the same realtime,
zero-reorder-latency budget. Managed via `POST /v1/transcoding/source-jobs`
(and GET/DELETE), or the admin UI's **Source Transcode** tab. Only the software
backend with H.264/AAC is accepted — other codecs/backends are disabled in the
UI and rejected by the API until built. The RTMP pull path uses native
DNS/TCP, handshake, AMF/chunk decoding, acknowledgement and ping handling;
libcurl is used only for HTTP(S) HLS/TS fetching. Encrypted, byte-range and
fragmented-MP4 HLS playlists are rejected immediately with a precise job
detail because the native demuxer currently accepts MPEG-TS segments.

Or during a VPS install: `RTMP_ENABLE_NATIVE_TRANSCODE=1 bash scripts/install-linux.sh`.

### How a source job behaves at runtime

These are the properties an unattended VPS deployment depends on; each is
enforced in `native/source_job_manager.cpp` or `native/hls_source_puller.cpp`.

**Joining a live source.** The first playlist poll ingests only the trailing
segments needed to fill the publisher's cold-start runway (≈35 s, minimum
three segments). Many IPTV panels advertise a window minutes deep; ingesting
all of it would put that entire backlog through the transcoder and leave every
viewer permanently that far behind the broadcast. A VOD source
(`EXT-X-ENDLIST`) is still played from its beginning.

**Staying at the live edge.** After the initial join, a poll that turns up more
than 60 s of un-ingested media means the pipeline is behind real time. The
puller skips the oldest of that backlog and emits `EXT-X-DISCONTINUITY` at the
jump rather than transcoding history. `PacedSegmentPublisher` separately bounds
its own queue (`max_buffer`, default 45 s): past that bound it releases each
segment at 90% of its media duration, so a surplus handed over by a bursty
source is absorbed over a few minutes instead of becoming permanent latency.

**Segment fetching.** All un-ingested segments in a poll are fetched
concurrently by a fixed four-worker pool whose libcurl handles live for the
whole job, so connections to the source's CDN are reused across segments and
polls. Demuxing stays strictly in playlist order — only the network fetch is
parallel.

**Restarts never give up.** A puller that reports `Error` is rebuilt after
`restart_delay_seconds`, doubling per consecutive failure up to
`restart_backoff_cap_seconds` (60 s) — and then retried at that interval
indefinitely. A source that is down for an hour comes back on its own when the
upstream returns; no operator action is needed. The failure streak is forgiven
once a puller has held `Running` for `healthy_reset_seconds` (120 s).

**A restart does not take the stream off the air.** Each rendition's
`SegmentStore` and its HLS registration outlive the puller, so the last live
window keeps serving while the pipeline is rebuilt, and the media sequence
resumes instead of colliding with already-cached segment URLs. Source-job
stores also set `repeat_last_segment_on_stall`, so the playlist keeps
advancing through an upstream outage instead of freezing (synthetic repeats
are excluded from `real_segments_added`, so stall detection still works).
Disabling a job is the one case that does unregister its outputs — that is an
operator decision to take the stream offline.

**CPU is divided between jobs.** `SourceJobManager` gives each starting job
`hardware_concurrency() / enabled_jobs` cores, which `SourceTranscoder` then
splits across that job's renditions. Without it, every job sized itself from
the full core count independently and four two-rendition jobs on an 8-core box
asked for ~32 encoder threads, pushing *all* of them behind real time.

**Management calls never block on a hung source.** Stopping a puller joins a
worker that may be inside a libcurl read for tens of seconds. Every path that
stops one (`remove`, `set_enabled(false)`, `restart`, auto-restart,
`stop_all`) detaches it under the manager mutex and joins it after releasing
that mutex, so `GET /v1/transcoding/source-jobs` stays responsive.

The pure geometry, x265 and AAC parameter-mapping logic (`native/geometry.cpp`,
`native/hevc_params.cpp`, `native/aac_params.cpp`) compiles and is unit-tested
**even without** the codec libraries; only the decode/scale/encode wrappers
require them. When the
option is on, `RTMP_NATIVE_TRANSCODE` is defined for consumers.

## Components

| File | Responsibility |
|------|----------------|
| `native/frame.hpp` | Owned, reusable I420 frame (the intermediate representation) |
| `native/geometry.*` | Pure FitMode → scale/crop/pad plan (testable, no libraries) |
| `native/hevc_params.*` | Pure preset → x265 parameter set (testable, no libraries) |
| `native/h264_decoder.*` | openh264 Annex B → I420 |
| `native/hevc_decoder.*` | libde265 Annex B → I420 (HEVC sources; see licensing note below) |
| `native/scaler.*` | libyuv scale + crop/letterbox execution of a `ScalePlan` |
| `native/hevc_encoder.*` | x265 I420 → HEVC Annex B access units |
| `native/video_transcoder.*` | Video glue: FLV sample → HEVC access units |
| `native/aac_params.*` | Pure preset → AAC encoder parameters (testable) |
| `native/aac_decoder.*` | libfdk-aac raw AAC → S16 PCM |
| `native/aac_encoder.*` | libfdk-aac PCM → ADTS AAC (internal frame buffering) |
| `native/audio_transcoder.*` | Audio glue: FLV AAC frame → ADTS access units |

## Integration boundary

`NativeVideoTranscoder` produces HEVC Annex B access units with 90 kHz PTS/DTS
and a keyframe flag — the exact shape `media::ts::TsMuxer::write_video`
consumes. Wiring these into the live RTMP fanout additionally requires HEVC
carriage in the origin's TS/enhanced-RTMP path (the current `TsMuxer` only
advertises the H.264 stream type). That packaging/fanout wiring is the next
step; this module delivers the decode→scale→encode core it plugs into.
