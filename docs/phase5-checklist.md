# Phase 5 Implementation Checklist — Media Ingest

- [x] RTMP audio messages — `MediaIngest::on_audio_message`, parses the
  FLV audio tag header (SoundFormat/AACPacketType)
- [x] RTMP video messages — `MediaIngest::on_video_message`, parses the
  FLV video tag header (FrameType/CodecID/AVCPacketType/composition time)
- [x] metadata — `MediaIngest::on_metadata_message`, decodes AMF0 Data
  (type 18) payloads and recognizes `@setDataFrame`/`onMetaData`
- [x] H.264 sequence-header parsing — `parse_avc_sequence_header`, full
  AVCDecoderConfigurationRecord (ISO 14496-15), retains all SPS/PPS entries
- [x] AAC sequence-header parsing — `parse_aac_sequence_header`, decodes
  AudioSpecificConfig object type/sample rate index/channel config, retains
  raw bytes verbatim
- [x] keyframe detection — FrameType nibble (1=key, 4=generated-key) tracked
  per stream (`StreamMediaState::seen_keyframe`, `MediaStats::
  keyframe_count`/`last_keyframe_timestamp`)
- [x] timestamps — `RtmpMessage::timestamp` recorded per audio/video message
  and per keyframe (`MediaStats::last_audio_timestamp`/
  `last_video_timestamp`/`last_keyframe_timestamp`)
- [x] statistics — `MediaStats`: message counts (audio/video/metadata/
  rejected), cumulative byte counts, keyframe count
- [x] Wired into `CommandSession::handle_message` — Audio(8)/Video(9)/
  Amf0Data(18) routed to an injected `MediaIngest*` for streams currently
  `NetStreamState::Publishing`
- [x] Tests — 16 new GoogleTest cases (15 `MediaIngestTest` + 1
  `CommandSessionTest` routing case), see below

## Files created

- `include/rtmp_server/protocol/media/media_ingest.hpp`
- `src/protocol/media/media_ingest.cpp`
- `tests/protocol/media/media_ingest_test.cpp`
- `docs/media-ingest.md`
- `docs/phase5-checklist.md` (this file)

## Files changed

- `include/rtmp_server/protocol/commands/command_session.hpp` — added
  `#include "rtmp_server/protocol/media/media_ingest.hpp"`,
  `set_media_ingest()`, private `route_media_message()`, and the
  `media_ingest_` non-owning pointer member; updated `handle_message`'s
  doc comment
- `src/protocol/commands/command_session.cpp` — `handle_message` now
  branches on Audio(8)/Video(9)/Amf0Data(18) into the new
  `route_media_message()` before falling through to the existing
  Amf0Command(20) dispatch path
- `src/protocol/CMakeLists.txt` — added `media/media_ingest.cpp` to
  `rtmp_server_protocol`
- `tests/protocol/CMakeLists.txt` — added `media/media_ingest_test.cpp` to
  `rtmp_server_protocol_tests`
- `tests/protocol/commands/command_session_test.cpp` — added
  `AudioVideoMetadataAreRoutedToMediaIngestOnlyWhilePublishing`

`MediaIngest` was placed under `include/rtmp_server/protocol/media/` (new
directory, mirroring `protocol/commands/`, `protocol/chunk/`,
`protocol/amf0/`) rather than under the existing (currently-empty)
`include/rtmp_server/media/` directory: that top-level `media/` is reserved
for transport/codec-adjacent concerns per the existing tree layout (sibling
to `network/`, `io/`, `recording/`), while this component is pure protocol
parsing with zero transport dependency, same category as
`protocol::commands::CommandSession` and `protocol::chunk::ChunkDecoder` —
see `docs/media-ingest.md` for the full reasoning, including why per-stream
retained state (`StreamMediaState`) lives inside `MediaIngest` itself
keyed by `stream_key` rather than being added to
`commands::StreamRegistration`.

## Acceptance criteria evidence

- **OBS publishes H.264/AAC** — no live OBS available on this host, same
  deferral prior phases used. Verified instead with GoogleTest cases
  constructing `chunk::RtmpMessage`s carrying real, byte-for-byte encoded
  FLV/RTMP audio and video tags: a full AVCDecoderConfigurationRecord with
  real profile(0x64)/level(0x1F)/SPS/PPS byte values
  (`avc_sequence_header_payload()`), a real AVC NALU keyframe tag with an
  IDR NAL unit type byte (`avc_keyframe_nalu_payload()`), and the
  well-known two-byte AAC-LC 44.1kHz-stereo AudioSpecificConfig `0x12 0x10`
  (`aac_sequence_header_payload()`) — matching what ffmpeg/OBS AAC encoders
  actually produce on the wire.
- **metadata is parsed** — `MediaIngestTest.MetadataOnMetaDataIsCounted`
  (the `@setDataFrame`/`onMetaData` wrapped shape OBS sends) and
  `MediaIngestTest.PlainOnMetaDataWithoutSetDataFrameWrapperIsCounted` (the
  unwrapped shape some encoders send) both assert
  `MediaStats::metadata_message_count == 1`.
- **SPS/PPS are retained** —
  `MediaIngestTest.AvcSequenceHeaderRetainsSpsAndPps`: after feeding a real
  AVCDecoderConfigurationRecord video tag, asserts
  `StreamMediaState::avc_sequence_header` is populated with exactly 1 SPS
  (7 bytes) and 1 PPS (4 bytes), plus correct profile/level/
  `nalu_length_size` fields.
- **AAC configuration is retained** —
  `MediaIngestTest.AacSequenceHeaderIsRetainedAndDecoded`: asserts
  `aac_sequence_header` decodes `object_type == 2` (AAC-LC),
  `sampling_frequency_index == 4` resolved to `sampling_frequency ==
  44100`, `channel_configuration == 2`.
  `MediaIngestTest.RawAacFrameDoesNotOverwriteRetainedSequenceHeader`
  confirms a subsequent raw AAC frame doesn't clear the retained config.
- **keyframes are detected** —
  `MediaIngestTest.KeyframeIsDetectedFromNaluFrameTypeNibble`: an
  inter-frame NALU tag leaves `seen_keyframe == false`, a subsequent
  keyframe NALU tag sets `seen_keyframe == true`,
  `stats.keyframe_count == 1`, and `stats.last_keyframe_timestamp` matches
  the message's timestamp.
  `MediaIngestTest.AvcSequenceHeaderRetainsSpsAndPps` additionally confirms
  the sequence-header tag itself (also flagged keyframe by FrameType) sets
  `seen_keyframe`.
- **malformed packets are rejected** — 7 dedicated cases:
  `RejectsEmptyAudioPayload`, `RejectsEmptyVideoPayload`,
  `RejectsTruncatedAvcSequenceHeader`,
  `RejectsAvcSequenceHeaderWithTruncatedSpsLength`,
  `RejectsTruncatedAacSequenceHeader`,
  `RejectsAudioTagMissingAacPacketTypeByte`,
  `RejectsMalformedMetadataPayload` — each asserts `Result::ok() == false`
  and `MediaStats::rejected_message_count` incremented, with no crash.

## Build and test evidence

Host: macOS/Darwin (no `io_uring`/Linux available), so the `core-only`
CMake preset (`RTMP_SERVER_CORE_ONLY=ON`) was used, same as every prior
phase.

```
$ cmake --preset core-only
$ cmake --build --preset core-only
[6/6] Linking CXX executable tests/protocol/rtmp_server_protocol_tests
$ ctest --preset core-only
100% tests passed out of 108
Total Test time (real) =   0.94 sec
```

108 total: 92 pre-existing (Phase 0-4) + 16 new Phase 5 tests
(`MediaIngestTest` x15, `CommandSessionTest.
AudioVideoMetadataAreRoutedToMediaIngestOnlyWhilePublishing` x1).

Also configured and built the `asan` preset with
`RTMP_SERVER_CORE_ONLY=ON` (`-fsanitize=address,undefined`):

```
$ cmake --preset asan -DRTMP_SERVER_CORE_ONLY=ON
$ cmake --build --preset asan
[6/6] Linking CXX executable tests/protocol/rtmp_server_protocol_tests
$ ctest --preset asan
100% tests passed out of 108
Total Test time (real) =   4.92 sec
```

Same 108/108 pass clean under ASan+UBSan — no leaks, no undefined behavior,
no heap corruption, including the truncated-length/short-payload rejection
paths (which read close to `payload.size()` boundaries) and the
`std::span`-based AVCDecoderConfigurationRecord/AudioSpecificConfig
parsers.

## Known limitations

See `docs/media-ingest.md` "Known limitations" for the full list; summary:

- NALU-internal `nal_unit_type` is not inspected — keyframe detection
  relies on the FLV `FrameType` nibble only (matches real encoder
  behavior).
- AudioSpecificConfig extended/SBR forms are not decoded past the first
  two bytes (raw bytes are still retained in full).
- AVC composition time offset is validated for presence but not decoded —
  belongs to Playback (Phase 7).
- No live OBS/ffmpeg instance was available; acceptance verified via
  GoogleTest cases with real encoded byte sequences, same deferral pattern
  as Phase 2/4.
- Not wired into `event_loop.cpp`/`IoUringEventLoop` — same deferral as
  every prior phase; no real socket transport exists on this host.
- `StreamRegistry` is untouched by this phase (see `docs/media-ingest.md`
  "Where per-stream media state lives" for why `StreamMediaState` lives in
  `MediaIngest` instead) — a later phase may choose to have `StreamRegistry`
  hold a handle into `MediaIngest` instead of two independent key lookups.

## Security concerns

- All parsing (`parse_avc_sequence_header`, `parse_aac_sequence_header`,
  audio/video tag header reads) does explicit length checks before every
  indexed/offset read of attacker-controlled (network) input — verified
  under ASan/UBSan (no out-of-bounds reads, no signed-overflow UB) with the
  truncated-length test cases specifically targeting the offset arithmetic.
- `MediaIngest` never allocates unbounded memory from a single message: SPS/
  PPS/AudioSpecificConfig sizes are bounded by the message payload length
  itself (which upstream `ChunkDecoder`/message-size limits already cap),
  no separate untrusted length field is trusted to allocate ahead of
  validating it fits within the payload.
- A malformed audio/video/metadata message never tears down the
  connection or the previously-retained sequence header/stats for that
  stream — only that single message is rejected and counted, limiting the
  blast radius of a single bad packet (matches `ChunkDecoder`'s own
  malformed-input policy).

## Performance concerns

- Per-message work is O(payload size) with no extra copies beyond the
  necessary SPS/PPS/`raw` byte retention (which happens only on sequence-
  header messages, not on every frame) — steady-state raw audio/video
  frames only touch header bytes and update counters.
- `MediaIngest::streams_` is an `unordered_map<std::string, ...>` keyed by
  stream key with no locking (unlike `StreamRegistry`, which is shared
  across connections and mutex-guarded) — `MediaIngest` as currently
  wired is used from a single `CommandSession`'s message-handling path, so
  no concurrent access exists yet. If a later phase shares one
  `MediaIngest` instance across multiple connections/threads (e.g. for
  Playback to read retained sequence headers concurrently with ingest
  writing them), it will need the same mutex treatment `StreamRegistry`
  already has.

## Next phase

Next: Phase 6 — FLV Recording.
