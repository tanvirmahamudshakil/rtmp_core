# Phase 6 Implementation Checklist — FLV Recording

- [x] FLV header — `media::flv::encode_file_header` (13 bytes: "FLV"+version+
  TypeFlags+DataOffset=9 + PreviousTagSize0=0), byte-exact test
- [x] metadata tags — `media::flv::build_onmetadata_tag` (AMF0 "onMetaData" +
  ECMA array), with duration/filesize written as placeholders and patched at
  finalize; recorder extracts width/height/framerate/codec ids from the
  publisher's onMetaData via the Phase-5 AMF0 decoder
- [x] audio tags — `Recorder::on_audio`, verbatim passthrough of the RTMP
  audio (type 8) payload framed as an FLV audio tag (0x08) with running
  PreviousTagSize
- [x] video tags — `Recorder::on_video`, same for RTMP video (type 9) → FLV
  video tag (0x09)
- [x] async file writer — `io::io_uring::IoUringFileSink`, real
  `IORING_OP_WRITE` writes through an owned io_uring ring (no blocking
  write()/fwrite()), bounded in-flight queue; abstracted behind
  `recording::FileSink` so the recorder is testable without io_uring
- [x] finalization — `Recorder::finalize` (idempotent): ensures header+
  metadata, patches duration/filesize placeholders, flushes+fsyncs+closes;
  called from `CommandSession` on deleteStream and connection close, including
  abrupt disconnect
- [x] FLV inspector — `apps/flv_inspector`, reads a .flv back and prints
  header + tag list (type/timestamp/size), doubling as playability evidence
- [x] tests — 18 new GoogleTest cases (9 `FlvWriterTest` + 8 `RecorderTest` +
  1 `CommandSessionTest` wiring case), see below

## Files created

- `include/rtmp_server/media/flv/flv_writer.hpp`
- `src/media/flv/flv_writer.cpp`
- `include/rtmp_server/recording/file_sink.hpp` (abstract FileSink)
- `include/rtmp_server/recording/recorder.hpp`
- `src/recording/recorder.cpp`
- `include/rtmp_server/protocol/commands/recorder_sink.hpp` (the abstract hook
  CommandSession routes into — keeps protocol → recording dependency-free)
- `include/rtmp_server/io/io_uring/file_sink.hpp` (Linux-only)
- `src/io/io_uring/file_sink.cpp` (Linux-only)
- `apps/flv_inspector/main.cpp`, `apps/flv_inspector/CMakeLists.txt`
- `src/media/CMakeLists.txt` (rtmp_server_media = flv + recording)
- `tests/media/flv_writer_test.cpp`, `tests/media/CMakeLists.txt`
- `tests/recording/recorder_test.cpp`, `tests/recording/CMakeLists.txt`
- `docs/flv-recording.md`
- `docs/phase6-checklist.md` (this file)

## Files changed

- `include/rtmp_server/protocol/commands/command_session.hpp` — added
  `#include "recorder_sink.hpp"`, `set_recorder()`, `RecorderSink* recorder_`
- `src/protocol/commands/command_session.cpp` — `route_media_message` now also
  forwards Audio/Video/metadata to the recorder while Publishing;
  `handle_delete_stream` and `on_connection_closed` call `recorder_->finalize()`
- `src/io/io_uring/CMakeLists.txt` — added `file_sink.cpp` to
  `rtmp_server_io_uring`
- `CMakeLists.txt` (root) — added `add_subdirectory(src/media)`,
  `apps/flv_inspector`, `tests/media`, `tests/recording`
- `tests/protocol/commands/command_session_test.cpp` — added
  `MediaIsRoutedToRecorderWhilePublishingAndFinalizedOnClose`

## Architecture decisions

- **Three-layer split (format / orchestration / sink).** FLV byte-format code
  lives under `media/flv/` and recording orchestration under `recording/`, per
  the reserved directories. The actual io_uring writes are behind an abstract
  `FileSink` so the recorder builds and is fully tested on this macOS host
  (core-only), while the production `IoUringFileSink` stays Linux-gated in the
  io_uring target — same deferral every prior phase used. Full rationale in
  `docs/flv-recording.md`.
- **No protocol → recording dependency cycle.** `CommandSession` routes to the
  recorder through `protocol::commands::RecorderSink` (abstract, in the
  protocol library), implemented by `recording::Recorder` (which depends on the
  protocol library for AMF0/chunk). The injected non-owning pointer mirrors the
  Phase-5 `set_media_ingest` wiring.
- **onMetaData placeholder-and-patch.** duration/filesize are written as
  fixed-width 8-byte AMF0 placeholders and rewritten in place at finalize (via
  `FileSink::patch`), since a Number is exactly 8 bytes and can be overwritten
  without shifting following bytes. Chosen over "minimal onMetaData" because
  duration/filesize are the fields players/scrub bars actually use.
- **Bounded queue = drop-newest against a byte budget** (default 16 MiB), not
  publisher backpressure: backpressuring one publisher would stall the shared
  single-threaded event loop, and drop-oldest would corrupt committed file
  offsets. Drop-newest keeps a valid, monotonically-timestamped prefix.

## Build commands

Host is macOS/Darwin (no io_uring/Linux), so the `core-only` preset is used,
same as every prior phase. (`scripts/build-debug.sh`/`run-tests.sh` target the
Linux `debug` preset, which needs liburing; the core-only preset is the
macOS-buildable equivalent used throughout this project.)

```
$ cmake --preset core-only
$ cmake --build --preset core-only
```

## Test commands

```
$ ctest --preset core-only
```

## Actual test results

```
100% tests passed out of 126
Total Test time (real) =   1.10 sec
```

126 total: 108 pre-existing (Phase 0–5) + 18 new Phase 6 tests
(`FlvWriterTest` ×9, `RecorderTest` ×8, `CommandSessionTest.
MediaIsRoutedToRecorderWhilePublishingAndFinalizedOnClose` ×1).

## Sanitizer results

Configured/built the `asan` preset with `RTMP_SERVER_CORE_ONLY=ON`
(`-fsanitize=address,undefined`):

```
$ cmake --preset asan -DRTMP_SERVER_CORE_ONLY=ON
$ cmake --build --preset asan
$ ctest --preset asan
100% tests passed out of 126
Total Test time (real) =   5.66 sec
```

126/126 clean under ASan+UBSan — no leaks, no undefined behavior, including
the FLV parser's offset arithmetic (truncated-tag/bad-signature rejection
cases) and the recorder's placeholder-patch offset math.

## Acceptance criteria evidence

- **recorded FLV is playable** — no real media player on this host (macOS),
  same deferral Phase 2/4/5 documented for OBS. Verified instead by reading
  the recording back byte-for-byte: `apps/flv_inspector` parses a real
  on-disk `.flv` produced by the FLV writer and prints a well-formed header
  and tag list (script/video/audio, correct DataSize + PreviousTagSize = 11 +
  DataSize), and `FlvWriterTest`/`RecorderTest` assert byte-exact header and
  tag encoding plus successful `parse_flv` round-trip
  (`RecorderTest.ProducesValidFlvWithHeaderMetadataAndMediaInOrder`). Sample
  inspector run:
  ```
  FLV version=1 audio=yes video=yes data_offset=9 prev_tag_size0=0 bytes=303
  #    type    timestamp    data_size  prev_size
  0    script  0            220        231
  1    video   0            3          14
  2    audio   0            4          15
  3    video   33           3          14
  total tags: 4
  ```
- **timestamps remain valid** —
  `RecorderTest.TimestampsArePreservedIncludingExtendedRange`: a 500 ms tag
  and a > 24-bit (0x01000000) tag both round-trip exactly through the
  TimestampExtended byte; `FlvWriterTest.AppendTagCarriesExtendedTimestamp`
  asserts the extended byte encoding directly.
- **abrupt publisher disconnect finalizes safely** —
  `RecorderTest.AbruptDisconnectFinalizesSafelyEvenWithNoMedia` (finalize with
  zero media still yields a valid header+onMetaData FLV),
  `RecorderTest.FinalizeIsIdempotent` (repeated finalize closes the sink once),
  and `CommandSessionTest.MediaIsRoutedToRecorderWhilePublishingAndFinalizedOnClose`
  (`on_connection_closed` finalizes exactly once).
- **recording queue is bounded** —
  `RecorderTest.BoundedQueueDropsFramesWhenSinkIsStalled`: with the sink
  reporting a large `pending_bytes()` (stuck disk) and a 1 KiB budget, incoming
  frames are dropped (`dropped_frames`/`dropped_bytes` increment), the file
  does not grow, `failed` stays false, and recording resumes once the sink
  drains.
- **disk failures do not crash server** —
  `RecorderTest.DiskFailureDoesNotCrashAndIsRecorded`: every `FileSink::append`
  returns a Storage error; the recorder swallows it, sets `stats().failed`,
  never throws, and finalize still closes the sink exactly once. All paths
  return `core::Result` — no unchecked syscall returns, no exceptions into the
  io_uring completion path (`IoUringFileSink` treats negative completion res /
  short writes as logged failures, not crashes).

## Known limitations

See `docs/flv-recording.md` "Known limitations"; summary:

- Late onMetaData updates (after media has started) are ignored, not rewritten.
- `IoUringFileSink` is not exercised by CI on this host (macOS, no io_uring) —
  covered via the shared `FileSink` contract (in-memory fake + real sink) and
  byte-for-byte format verification. Same deferral as every prior phase.
- Not wired into `event_loop.cpp`/`IoUringEventLoop` — no real socket
  transport on this host; connecting a live `CommandSession` to an
  `IoUringFileSink`-backed `Recorder` is the remaining Linux integration step.
- One `Recorder` = one stream = one file (typical one-publish-per-connection).

## Security concerns

- The FLV parser (`parse_flv`, used by the inspector and by tests on
  attacker-influenceable file bytes) does explicit bounds checks before every
  offset read and rejects bad signature / truncated header / a tag whose
  DataSize+PreviousTagSize runs past the buffer — verified under ASan/UBSan.
- Recording never allocates unbounded memory: the bounded queue caps
  outstanding bytes and drops rather than growing; per-tag buffers are bounded
  by the RTMP message size upstream `ChunkDecoder` already caps.
- A disk failure is contained to the recording — it never tears down the
  connection or the publisher, and never escapes as an exception into the I/O
  completion path.
- Structured logs redact as required: only byte counts / error strings /
  offsets are logged, never stream keys or payload contents.

## Performance concerns

- Steady-state per tag is one buffer copy (into the in-flight write buffer)
  plus one SQE — no re-encoding of codec bytes (audio/video data is verbatim
  passthrough). onMetaData is built once.
- `IoUringFileSink` reaps completions opportunistically on each append and only
  blocks when it must make room (bounded-queue drain) or on patch/finalize, so
  the common path never blocks the caller.
- The finalize `patch` rewrites just 16 bytes (two doubles) — O(1), no file
  rewrite.

## Next phase

Next: Phase 7 — RTMP Playback (subscriber session, GOP cache, metadata/codec
startup, live fan-out, viewer backpressure, slow-client handling, disconnect
cleanup).
