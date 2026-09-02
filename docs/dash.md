# MPEG-DASH delivery

MPEG-DASH packaging of the same RTMP ingest that produces this server's HLS
output, served at `/dash` alongside `/hls`. Off by default
(`dash_enabled: false`); a stream is packaged into DASH only when the flag is
on, and then in parallel with HLS from the same publish — not instead of it.

## Why a separate module, not an HLS extension

HLS and DASH need different containers. HLS stays on MPEG-TS (see
`docs/hls.md` "Container choice") for segment independence and reconnect
robustness; DASH's live profile (`isoff-live`) is defined over fragmented
MP4/CMAF. Rather than bolt fMP4 packaging onto `hls::Segmenter`, this is a
parallel pipeline, `dash::Segmenter`, sharing only the underlying codec
helpers (`media::h264`, `media::aac`) and the same muxer
(`media::mp4::Fmp4Muxer`) that a future LL-HLS-over-CMAF profile could also
use. `TeeRecorderSink` fans one publisher's media out to both segmenters (and
the FLV recorder) without re-parsing it per consumer.

## Container: fMP4, not the TS path's Annex B / ADTS

Unlike the TS segmenter, `dash::Segmenter` does **no bitstream conversion**:
video samples stay exactly the length-prefixed AVCC layout FLV delivers, and
audio stays raw AAC access units with no ADTS header — `media::mp4::Fmp4Muxer`
wants precisely that shape, because a fragmented MP4 sample entry (`avcC`)
carries the parameter sets once, in the init segment, not per frame.

## Pipeline

```
RTMP publish
    |
CommandSession -> TeeRecorderSink -> { FLV Recorder, hls::Segmenter, dash::Segmenter }
                                                            |
                                            dash::SegmentStore (init + media segments)
                                                            |
                                         control::DashHttpHandler (/dash/*)
```

`dash::Segmenter` cuts on the same wall-clock cadence as the HLS segmenter (6 s
by default, keyframe-aligned, same trade-off documented in `docs/hls.md`
"Storage model") so both delivery surfaces advertise comparable live windows
for one publisher. It produces:

* an **init segment** (`ftyp`+`moov`) once the first valid H.264 SPS/PPS and
  AAC AudioSpecificConfig arrive, and again on a genuine mid-stream parameter
  change (a new epoch, `InitSegment::epoch`) — never on a repeated, unchanged
  sequence header, which some encoders send on every keyframe;
* one **media segment** (`styp`+`moof`+`mdat`) per cut, with sample durations
  derived from actual inter-frame timestamps (a one-sample lookahead buffers
  each frame until its successor arrives to know its real duration), not
  assumed from the configured target — an assumed duration would drift a
  player's presentation clock over a long live session.

## HTTP delivery

Routes, all GET/HEAD, under `control::DashHttpHandler`:

```
{prefix}/{application}/{stream}/manifest.mpd
{prefix}/{application}/{stream}/{representation}/init.mp4
{prefix}/{application}/{stream}/{representation}/{name}.m4s
```

The manifest uses `SegmentTemplate`/`$Number$` addressing (RFC-equivalent:
ISO/IEC 23009-1 5.3.9.4.4), matching the same fixed-cadence, sequence-numbered
model HLS already uses — no `SegmentTimeline`, so a GOP whose length is not an
exact divisor of the target duration produces a segment of slightly different
real length, the same simplification HLS's own segmenter makes. Segments are
immutable and safely cacheable exactly like HLS TS segments; the manifest
itself is short-cached (`no-cache, max-age=0`), and the init segment is
revalidatable rather than permanently immutable, since a codec-parameter
change republishes a new body at the *same* init URL — DASH's
`SegmentTemplate@initialization` has no place to encode an epoch the way an
HLS `EXT-X-DISCONTINUITY` can.

The multi-node edge/origin-shield token (`hls_edge_fetch_secret`,
`X-Edge-Token`) gates `/dash` too, so one edge tier protects both delivery
surfaces with one secret.

## Known limitations

* One multiplexed representation per rendition (video+audio together), not
  split video/audio `AdaptationSet`s — matches how this server's HLS
  renditions already work, and keeps ABR logic identical across both
  surfaces. Splitting them (for independent audio-only ABR) is unimplemented.
* `Representation@bandwidth` is a fixed planning figure, not measured live
  throughput — same gap the HLS master playlist has (`docs/hls.md` "Multiple
  renditions").
* Low-Latency DASH (chunked CMAF, small chunks over a stream) is not
  implemented; DASH here is the plain live profile only.
* No DASH-side segment encryption (`ContentProtection`) or DRM. HLS's
  AES-128 `EXT-X-KEY` has no DASH counterpart yet.
* H.264 + AAC passthrough only, same restriction as `hls::Segmenter`; no
  HEVC representations.
