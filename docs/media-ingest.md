# Media Ingest

> Status: implemented in Phase 5. See `docs/phase5-checklist.md` for the
> phase tracking record.

## Scope

Consumes RTMP Audio (type 8), Video (type 9), and AMF0 Data (type 18,
`@setDataFrame`/`onMetaData`) messages for a stream that is currently
`Publishing`, per `docs/rtmp_promot.md` "Phase 5: Media Ingest". Implemented
by:

* `rtmp_server::protocol::media::MediaIngest`
  (`include/rtmp_server/protocol/media/media_ingest.hpp`,
  `src/protocol/media/media_ingest.cpp`) — pure parsing/bookkeeping
  component, no sockets, no io_uring, consumes already-reassembled
  `chunk::RtmpMessage`s, same architectural shape as
  `protocol::commands::CommandSession` and `protocol::handshake::
  HandshakeSession` (docs/architecture.md "Architectural Separation").
* Free functions `parse_avc_sequence_header` / `parse_aac_sequence_header`
  — exposed separately from the class so a later phase (FLV recording,
  Phase 6) can reuse the same container parsers without going through the
  per-stream state machine.

Explicitly out of scope for this phase (per `docs/rtmp_promot.md`): FLV
file writing, GOP caching for playback, actually decoding H.264/AAC bitstream
content (SPS bit-field parsing beyond profile/level/length-size, frame
reassembly, RTP/other output). This component only does *container-level*
parsing: figuring out which bytes are the sequence header vs. a raw frame,
extracting SPS/PPS/AudioSpecificConfig, and flagging keyframes.

## FLV/RTMP tag formats parsed

### Audio tag (message type 8)

```
byte 0: SoundFormat(4) SoundRate(2) SoundSize(1) SoundType(1)
if SoundFormat == 10 (AAC):
  byte 1: AACPacketType (0 = AudioSpecificConfig, 1 = raw AAC frame)
  bytes 2..: AudioSpecificConfig (if AACPacketType==0) or raw AAC data
```

### Video tag (message type 9)

```
byte 0: FrameType(4) CodecID(4)
if CodecID == 7 (AVC):
  byte 1: AVCPacketType (0 = AVCDecoderConfigurationRecord, 1 = NALU, 2 = end of sequence)
  bytes 2..4: composition time offset (24-bit signed, ignored by this phase)
  bytes 5..: AVCDecoderConfigurationRecord (if AVCPacketType==0) or one or more
             length-prefixed NALUs (if AVCPacketType==1)
```

`FrameType == 1` (key frame) or `4` (generated key frame, reserved for
server use) both count as a keyframe for `StreamMediaState::seen_keyframe`
and `MediaStats::keyframe_count`/`last_keyframe_timestamp` — matches how
real encoders occasionally emit type 4 and how FLV consumers are expected to
treat it identically to type 1 for seek-point purposes.

### AVCDecoderConfigurationRecord (ISO 14496-15 §5.2.4.1)

```
configurationVersion(1) AVCProfileIndication(1) profile_compatibility(1)
AVCLevelIndication(1) reserved(6)+lengthSizeMinusOne(2)(1)
reserved(3)+numOfSequenceParameterSets(5)(1)
{ sequenceParameterSetLength(2) sequenceParameterSetNALUnit }*
numOfPictureParameterSets(1)
{ pictureParameterSetLength(2) pictureParameterSetNALUnit }*
```

All SPS and PPS NALUs present are retained (`AvcSequenceHeader::sps_list` /
`pps_list`), not just the first — a conforming encoder may signal more than
one SPS/PPS (e.g. for adaptive resolution), and a later phase re-serving
this stream needs all of them, not an arbitrary subset.

### AudioSpecificConfig (ISO 14496-3 §1.6.2.1, simple form)

```
audioObjectType(5) samplingFrequencyIndex(4) channelConfiguration(4) ...
```

Only the first two bytes (audioObjectType, samplingFrequencyIndex,
channelConfiguration) are decoded into fields; the "extended" ASC forms
(SBR/PS signaling via `syncExtensionType`, more bytes) are not parsed since
nothing in this phase needs them — but the **exact raw bytes** are retained
verbatim in `AacSequenceHeader::raw` regardless, so a later phase resending
this sequence header to a new subscriber (Phase 7 Playback) does not lose
any information this parser doesn't itself understand.

## Where per-stream media state lives

`StreamMediaState` (retained SPS/PPS, AudioSpecificConfig, codec IDs,
`MediaStats`) is stored inside `MediaIngest` itself, keyed by `stream_key`
(a plain `std::string`, the same key `StreamRegistry` uses) — **not** added
as new fields on `commands::StreamRegistration`/`StreamRegistry`.

Reasoning, mirroring how Phase 4's checklist justified `StreamRegistry`'s
own placement:

* `MediaIngest` needs to be unit-testable on its own, the same way
  `CommandSession` and `StreamRegistry` are each independently testable
  without one requiring the other to exist. Folding `StreamMediaState` into
  `StreamRegistration` would mean every `MediaIngest` test has to first
  build a `StreamRegistry` and register a publisher, for no benefit to what
  `MediaIngest` itself is testing (AVCDecoderConfigurationRecord/
  AudioSpecificConfig parsing, keyframe detection, malformed-input
  rejection).
* `StreamRegistry` is explicitly documented (Phase 4 checklist) as staying
  "intentionally thin" and expecting *later* phases to extend it rather
  than replace it — but "extend" doesn't have to mean "add every future
  phase's fields to the one struct." A stream key is already the shared
  join key between the two components; `StreamRegistry` stays the
  authoritative answer to "who is publishing this key right now,"
  `MediaIngest` stays the authoritative answer to "what do we know about
  this key's media." A later phase (Playback) that needs both can look
  up each independently by the same `stream_key`, exactly like
  `CommandSession` already looks up `StreamRegistration` via `stream_key`
  today.
* This keeps `StreamRegistry` free of the `#include` on `media_ingest.hpp`
  (or vice versa) — the two components stay decoupled, matching how
  `ChunkDecoder` doesn't depend on `CommandSession` even though
  `CommandSession` depends on chunk types.

A later phase is free to have `StreamRegistry` hold a
`MediaIngest*`/shared handle instead of both being looked up separately by
key, if that turns out more convenient once Playback (Phase 7) needs to
join the two on every playback session start — that refactor is deferred,
not precluded.

## Wiring into CommandSession

`CommandSession::handle_message()` now branches on `message_type_id` before
the AMF0-command decode path: Audio(8)/Video(9)/Amf0Data(18) go to the new
private `route_media_message()`, which:

1. Looks up the `StreamSlot` for `message.message_stream_id`.
2. Requires `state == NetStreamState::Publishing` and a non-empty
   `stream_key` — media arriving before `publish()` completed (or on a
   stream ID that never published) is silently dropped, matching the
   pre-Phase-5 default behavior for these message types.
3. Forwards to the injected `media::MediaIngest*` (`set_media_ingest()`),
   keyed by the slot's `stream_key`.

`media_ingest_` is a raw, non-owning pointer defaulting to `nullptr` (like
`registry_` is a reference the caller owns) so `CommandSession` remains
constructible and testable without a `MediaIngest` instance when a test
doesn't care about media routing — see `CommandSessionTest.
AudioVideoMetadataAreRoutedToMediaIngestOnlyWhilePublishing` for the
positive case and the existing `NonAmf0CommandMessagesAreIgnored` for the
not-publishing-yet case (still passes unchanged: `Amf0Data` on message
stream ID 0, which trivially isn't `Publishing`, is still just dropped).

This intentionally does **not** wire `MediaIngest` into `event_loop.cpp`/
`IoUringEventLoop`, matching every prior phase's own deferral — there is no
real socket transport reachable on this host (macOS/Darwin, no io_uring).

## Malformed input handling

Every parse path returns `core::Result<T>` (or `Result<void>`) rather than
throwing or asserting, consistent with `amf0::decode`/`ChunkDecoder`'s own
handling of untrusted wire input. Rejections use
`core::ErrorCode::MalformedChunk` / `ErrorCategory::Protocol` with a
human-readable message. Rejected cases:

* Empty audio/video payload (no tag byte at all).
* AAC audio tag missing the `AACPacketType` byte (payload length < 2).
* AVC video tag missing `AVCPacketType`/composition-time bytes (payload
  length < 5).
* `AVCDecoderConfigurationRecord` shorter than its fixed 6-byte header.
* `AVCDecoderConfigurationRecord` whose SPS/PPS length fields claim more
  bytes than are actually present (truncated mid-record).
* `AVCDecoderConfigurationRecord` with zero SPS or zero PPS entries.
* `AudioSpecificConfig` shorter than 2 bytes.
* `AudioSpecificConfig` with `audioObjectType == 0` (reserved).
* Metadata payload that doesn't AMF0-decode to a `(String, ...)` value
  sequence at all, or a `@setDataFrame` wrapper missing its wrapped event
  name.

A rejection increments `MediaStats::rejected_message_count` for that stream
and leaves any previously-retained sequence header/stats untouched — a bad
message does not erase good prior state, matching how a single malformed
chunk in `ChunkDecoder` doesn't invalidate previously-reassembled messages.

## Statistics

`MediaStats` (per stream, inside `StreamMediaState`): audio/video/metadata
message counts, rejected message count, cumulative audio/video byte counts,
keyframe count, and the last audio/video/keyframe timestamp seen. Scoped to
what "statistics" reasonably means for an *ingest* component per the phase
spec's bullet list (`* statistics`) — bitrate-over-time, health/alerting,
and other observability concerns belong to
`docs/rtmp_promot.md`'s "Observability" section, not this component.

## Known limitations

* NALU-level parsing stops at "how many bytes is this length-prefixed
  NALU" — the payload of individual VCL/non-VCL NALUs inside a `Nalu`
  (AVCPacketType 1) video tag is not inspected (no re-detection of keyframe
  status from `nal_unit_type` within the NALU stream itself; keyframe
  detection relies entirely on the FLV `FrameType` nibble, which is what
  every RTMP encoder actually sets correctly — OBS/ffmpeg do not rely on
  in-band NALU type inspection for this).
* `AudioSpecificConfig` extended/SBR forms are not decoded into fields
  (only `raw` bytes are retained for those) — no ingest use case in this
  phase needs SBR/PS awareness.
* Composition time offset (3 bytes in the AVC video tag) is validated for
  presence (length check) but not decoded/stored — B-frame reordering is a
  Playback (Phase 7) concern.
* No live OBS/ffmpeg instance was available on this host; acceptance is
  verified with `chunk::RtmpMessage`s carrying real, byte-for-byte encoded
  FLV/RTMP audio and video tags (a real AVCDecoderConfigurationRecord with
  actual profile/level/SPS/PPS byte values, and the well-known two-byte AAC
  LC 44.1kHz stereo AudioSpecificConfig `0x12 0x10`), same deferral pattern
  Phase 2/4 used for a live OBS/ffmpeg client.
* Not wired into `event_loop.cpp`/`IoUringEventLoop` — same deferral as
  every prior phase.

Next: Phase 6 — FLV Recording.
