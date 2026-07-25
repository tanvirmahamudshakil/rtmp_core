# FLV Recording

> Status: implemented in Phase 6. See `docs/phase6-checklist.md` for the
> phase tracking record.

## Scope

Records a publishing RTMP stream to an on-disk FLV file, per
`docs/rtmp_promot.md` "Phase 6: FLV Recording". Three layers, deliberately
separated the same way Phase 5 separated `MediaIngest` from transport:

* **FLV byte-format** — `rtmp_server::media::flv`
  (`include/rtmp_server/media/flv/flv_writer.hpp`,
  `src/media/flv/flv_writer.cpp`). Pure encoding/parsing of the FLV container:
  file header, tag framing, the `onMetaData` script tag, and a read-back
  parser. No sockets, no io_uring, no recording policy. Builds on every
  platform (including `core-only`).
* **Recording orchestration** — `rtmp_server::recording::Recorder`
  (`include/rtmp_server/recording/recorder.hpp`,
  `src/recording/recorder.cpp`). Consumes the FLV-format audio/video/metadata
  payloads RTMP already delivers (same bytes `MediaIngest` parses), frames
  each as an FLV tag, enforces the bounded queue, patches header placeholders,
  and finalizes. Depends only on the abstract `FileSink` — no io_uring — so
  it is unit-testable everywhere.
* **Async file writer** — `recording::FileSink` (abstract,
  `include/rtmp_server/recording/file_sink.hpp`) with the production
  implementation `io::io_uring::IoUringFileSink`
  (`include/rtmp_server/io/io_uring/file_sink.hpp`,
  `src/io/io_uring/file_sink.cpp`). Linux-only, does the actual writes through
  `IORING_OP_WRITE`.

## Why the layer split

The master spec requires real file writes to go through io_uring, not
blocking `write()`/`fwrite()`. But io_uring is Linux-only and this host is
macOS (every prior phase deferred io_uring wiring for the same reason). If the
`Recorder` called liburing directly it could neither build nor be tested here.

So the `Recorder` depends on the abstract `FileSink`. Production wires it to
`IoUringFileSink`; tests wire it to an in-memory fake that can simulate a
stalled disk (for the bounded-queue path) and write failures (for the
disk-failure path). This is the same testability pattern the codebase already
uses (e.g. `CommandSession` takes a `StreamKeyValidator`/`OutgoingHandler`
rather than owning transport). It also keeps a clean dependency graph: the
recording library depends on the protocol library (for AMF0/chunk types); the
protocol library depends on neither — `CommandSession` routes to recording
through the abstract `protocol::commands::RecorderSink` interface, so there is
no dependency cycle.

## FLV format choices

### File header (13 bytes)

`"FLV"` + version 1 + TypeFlags + DataOffset(9), followed by
`PreviousTagSize0 = 0`. The recorder always sets **both** the audio and video
TypeFlags bits. The flags are advisory; players tolerate a flag set for an
absent track, but some demuxers mishandle a present track whose flag is clear,
so setting both is the safe default for a live recording whose exact track
set isn't known until data arrives.

### Tags

Each tag is the standard 11-byte FLV tag header
(`TagType`, `DataSize`(24), `Timestamp`(24) + `TimestampExtended`(8),
`StreamID`(24, always 0)), the tag data, then a trailing `PreviousTagSize`
= 11 + `DataSize`. Timestamps above 24 bits are carried in the
`TimestampExtended` byte exactly as the spec requires, so a recording longer
than ~4.6 hours keeps valid, monotonic timestamps (covered by
`RecorderTest.TimestampsArePreservedIncludingExtendedRange`).

Audio and video tag **data is a verbatim passthrough** of the RTMP
audio (type 8) / video (type 9) message payload. RTMP already delivers
AAC/AVC media in FLV tag-body form (the same `SoundFormat…`/`FrameType…`
leading bytes `MediaIngest` parses), so recording re-frames rather than
re-encodes — no transcoding, no re-muxing of the codec bytes.

### Metadata (`onMetaData`) — placeholder-and-patch

The onMetaData script tag is an AMF0 `String "onMetaData"` + `ECMA array`
of `duration`, `width`, `height`, `framerate`, `videocodecid`,
`audiosamplerate`, `audiosamplesize`, `stereo`, `audiocodecid`, `filesize`.

`duration` and `filesize` are not known until the file is closed. Two standard
approaches exist: (a) leave onMetaData minimal / omit them, or (b) write fixed
placeholders and patch them at finalize. **We chose (b).** `duration` and
`filesize` are the two fields players and scrub bars actually use, and a
fixed-width AMF0 `Number` is exactly 8 bytes, so it can be rewritten in place
without shifting any following bytes. `build_onmetadata_tag` returns the byte
offsets of those two doubles; the recorder records their absolute file offsets
and, at finalize, overwrites just those 16 bytes via `FileSink::patch`.

Metadata values (width/height/framerate/…) are extracted best-effort from the
publisher's `onMetaData`/`@setDataFrame` message by reusing the Phase-5 AMF0
decoder; a malformed metadata payload simply leaves the defaults in place and
never throws.

### Ordering guarantee

A valid FLV wants the file header first, then onMetaData, then media. OBS/
ffmpeg send `@setDataFrame(onMetaData)` before any media, but the recorder
does not rely on that: it writes the header lazily on the first tag, and
`ensure_metadata_written()` writes the onMetaData tag before the first media
tag regardless (using a received metadata message if one arrived, otherwise
defaults). So onMetaData is always tag 0, even if media arrives first or no
metadata is ever sent (`RecorderTest.ProducesValidFlvWithHeaderMetadataAndMediaInOrder`,
`…AbruptDisconnectFinalizesSafelyEvenWithNoMedia`).

## Async writer design (IoUringFileSink)

`IoUringFileSink` owns its own small io_uring ring (independent of the
connection event loop's ring). `append` copies the tag bytes into an in-flight
buffer, submits an `IORING_OP_WRITE` at the running file offset via a fresh
SQE, and reaps any ready completions non-blocking. Each in-flight buffer is
kept alive (keyed by a monotonic id in a `std::deque`) until its completion is
seen, so the kernel never reads freed memory. `patch` drains all in-flight
appends first (so the patch can't race an outstanding write), then does one
synchronous io_uring write it waits on. `finalize` drains everything, `fsync`s,
and closes the fd.

Writes use real io_uring ops end to end — there is no `write()`/`fwrite()` on
the data path.

## Bounded queue policy

An unbounded write queue lets a slow or stuck disk grow memory without limit
and OOM the server. Two options were on the table: backpressure the publisher,
or drop. **We chose drop-newest against a byte budget**, for two reasons:

1. **Backpressuring a single publisher would stall the shared event loop.**
   The server runs one single-threaded io_uring loop across all connections
   (`docs/architecture.md` Threading Model). Blocking that thread to wait for
   one publisher's disk would freeze every other connection. A recording is
   best-effort and must never become a head-of-line block on live traffic.
2. **Drop-newest keeps the file valid.** Dropping the *oldest* queued frame
   would leave a gap in the middle of already-computed file offsets and
   corrupt the running `PreviousTagSize`/timestamp sequence. Dropping the
   newest incoming frame leaves everything already committed a valid,
   monotonically-timestamped prefix — the recording is simply shorter/gappier
   near the stall, but always a playable FLV.

The bound is enforced at two composed layers: the `Recorder` checks
`FileSink::pending_bytes()` before each tag and drops (counting
`dropped_frames`/`dropped_bytes`) if the next tag would exceed
`RecorderConfig::max_queued_bytes` (default 16 MiB); the `IoUringFileSink`
additionally block-drains its own ring if in-flight bytes would exceed its
`max_inflight_bytes`. Covered by
`RecorderTest.BoundedQueueDropsFramesWhenSinkIsStalled`.

## Finalization strategy

`Recorder::finalize()` is idempotent and safe on every path — normal
`deleteStream`, connection close, or abrupt publisher disconnect (it is called
from both `CommandSession::handle_delete_stream` and
`CommandSession::on_connection_closed`). It:

1. Ensures the header + onMetaData exist (so even a zero-media recording is a
   valid FLV).
2. Patches `duration` (= max media timestamp / 1000, in seconds) and
   `filesize` (= total bytes written) into the onMetaData placeholders.
3. Calls `FileSink::finalize()` to flush/`fsync`/close — **always**, even if a
   prior write already failed, so the fd is released and whatever was written
   is durable.

`RecorderTest.FinalizeIsIdempotent` and
`…AbruptDisconnectFinalizesSafelyEvenWithNoMedia` cover the reentrancy and the
no-media case.

## Disk-failure handling

No syscall return value is unchecked and no exception is thrown on the I/O
path. `FileSink` methods return `core::Result`; on the first failure the
`Recorder` sets `stats().failed`, logs a structured `recorder/write_failed`
event, and stops feeding the sink — but never crashes and never propagates the
error into the caller (the RTMP hot path). `IoUringFileSink` treats a negative
completion `res` (ENOSPC/EIO/EPERM) or a short write as a failure, logs
`file_sink/write_failed`, and keeps the process alive.
`RecorderTest.DiskFailureDoesNotCrashAndIsRecorded` drives every write path to
failure and asserts no crash, `failed == true`, and that finalize still closes
the sink exactly once.

## Wiring into CommandSession

`CommandSession` gained `set_recorder(RecorderSink*)` (non-owning, optional),
mirroring `set_media_ingest`. In `route_media_message`, a stream that is
`Publishing` forwards Audio/Video/metadata to the recorder in addition to
`MediaIngest`. `handle_delete_stream` and `on_connection_closed` call
`recorder_->finalize()` for a publishing stream. `RecorderSink` is an abstract
interface in the protocol layer precisely so this wiring adds no link edge
from protocol to the recording library.

As with every prior phase, this is **not** wired into
`event_loop.cpp`/`IoUringEventLoop` — no real socket transport is reachable on
this macOS host. Connecting a live publisher's `CommandSession` to an
`IoUringFileSink`-backed `Recorder` is the remaining integration step for a
Linux deployment.

## Known limitations

* onMetaData is written once (before the first media tag). A late
  `onMetaData` update that arrives after media has started is ignored rather
  than rewritten — an in-place `patch` of individual numeric fields is
  possible but wasn't needed this phase.
* No B-frame composition-time rewriting; the RTMP video tag (including its
  24-bit composition time offset) is passed through verbatim, which is exactly
  what a faithful FLV recording should contain.
* `IoUringFileSink` is not exercised by CI on this host (macOS, no io_uring);
  it is covered by the abstract-`FileSink` contract the in-memory fake and the
  real sink both implement, plus byte-for-byte format verification via
  `flv_inspector`. Same deferral pattern as every prior phase.
* One `Recorder` records one stream to one file (the typical
  one-publish-per-connection case). A connection publishing multiple streams
  concurrently would need one `Recorder` per stream; the injected-pointer
  wiring records the first publishing stream.
