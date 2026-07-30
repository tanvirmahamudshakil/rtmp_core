# Native CPU H.265 (HEVC) transcoding

The stock transcoder shells out to FFmpeg (`docs/transcoding.md`). This module
is an **in-process, FFmpeg-free** alternative that produces HEVC renditions on
the CPU with a professional-grade quality-per-bitrate configuration.

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

Everything runs software-only: **openh264** decodes H.264, **libyuv** scales
I420, **x265** encodes HEVC. No GPU, no FFmpeg process.

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

Off by default. Enable with the CMake option after installing the dev packages:

```bash
sudo apt-get install libx265-dev libopenh264-dev libfdk-aac-dev libyuv-dev
cmake -S . -B build -DRTMP_ENABLE_NATIVE_TRANSCODE=ON
cmake --build build
```

Or during a VPS install: `RTMP_ENABLE_NATIVE_TRANSCODE=1 bash scripts/install-linux.sh`.

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
carriage in the origin's TS/enhanced-RTMP path (the current `TsMuxer` advertises
the H.264 stream type, and `build_arguments` in the FFmpeg supervisor still
rejects non-H.264 output). That packaging/fanout wiring is the next step; this
module delivers the decode→scale→encode core it plugs into.
