# PHASE 6 COMPLETION REPORT

Recording and optional HLS/LL-HLS output (`docs/v2_promot.md` lines 990–1073).

Branch `phase6-work`, branched from `836fbe0` ("Merge Phase 5: persistence,
authentication and management API").

---

## 1. What was inspected

- `docs/v2_promot.md` in full — sections 1–5 (engineering rules, working
  method, report format) and PHASE 6; PHASE 5 (856–986) and PHASE 7 (1077+)
  for boundary context.
- Existing recording stack: `include/rtmp_server/recording/file_sink.hpp`,
  `recorder.hpp`, `src/recording/recorder.cpp`,
  `tests/recording/recorder_test.cpp`.
- **Every** implementation of `FileSink` in the tree (an explicit audit
  requirement): `grep -rn "FileSink"` across `include/`, `src/`, `tests/`,
  `CMakeLists.txt`.
- `include/rtmp_server/io/io_uring/file_sink.hpp` + `src/io/io_uring/file_sink.cpp`.
- Recorder wiring: `src/protocol/commands/command_session.cpp` (lines 185–215,
  385, 402), `src/protocol/session/rtmp_connection_session.cpp:64`,
  `include/rtmp_server/protocol/commands/{command_session,recorder_sink}.hpp`.
- HTTP stack: `include/rtmp_server/control/http_server.hpp`,
  `src/control/http_server.cpp`, `management_api.hpp/.cpp`, `docs/management-api.md`.
- Phase 5 auth: `include/rtmp_server/authentication/rtmp_authenticator.hpp`,
  `include/rtmp_server/management/token.hpp`, `src/management/token.cpp`.
- Shared-buffer patterns: `core/buffer.hpp`, `protocol/commands/shared_media_frame.hpp`.
- FLV layer: `include/rtmp_server/media/flv/flv_writer.hpp` (including its
  `parse_flv` read-back parser, reused in tests).
- Root `CMakeLists.txt`, `CMakePresets.json`, and the `src/*/CMakeLists.txt`
  platform gating.
- Greenfield verification for HLS: `grep -ril "hls\|m3u8" include/ src/ tests/`
  → no matches.
- Tool availability: `which ffmpeg ffprobe` → **neither installed**.

---

## 2. Problems confirmed

1. **The only concrete `FileSink` was `IoUringFileSink`**, compiled solely
   under `NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux"`
   (root `CMakeLists.txt`). On a core-only/macOS build the abstract interface
   had **no production implementation at all** — only the in-memory test fake.
   Recording was therefore unavailable on the portable build.
2. **Nothing in production code ever constructed a `Recorder` or a
   `FileSink`.** `grep -rn "IoUringFileSink::open\|Recorder("` over `src/` and
   `apps/` matched only definitions. The recording library was complete and
   tested but entirely unwired.
3. **No disk-space monitoring** existed anywhere in the recording path.
4. **No retention** of any kind (age, count or total size).
5. **No atomic finalize.** `IoUringFileSink` writes directly to the final
   path; there is no temp-file + rename, so a crash mid-recording leaves a
   truncated file under the name a consumer would treat as finished.
6. **No HLS/LL-HLS code of any kind** — confirmed greenfield.
7. `Recorder` treated *any* sink `append` error as fatal, so a sink enforcing
   its own bounded queue would abort the whole recording on transient
   backpressure rather than dropping a frame.
8. `control::HttpServer::write_response` emitted only `"OK"`/`"Error"` as
   reason phrases and unconditionally wrote its own `Content-Length` —
   inadequate for `206`/`416` and for `HEAD` responses.

---

## 3. Problems not confirmed

1. **"Disk I/O blocks the RTMP thread" was NOT confirmed as a live defect.**
   On Linux, `IoUringFileSink` uses non-blocking `IORING_OP_WRITE` and is
   genuinely async. The real problem was the *absence* of any portable sink
   (item 1 above), and the risk that the obvious fix would introduce blocking
   I/O. So the phase task "move recording disk I/O off network event-loop
   threads" was addressed by building the portable path correctly from the
   start, not by repairing a broken existing one.
2. **`Recorder`'s bounded queue was already correct** — it checks
   `pending_bytes()` before framing and drops newest-first so the committed
   file stays a valid monotonic prefix. Not rewritten.
3. **`Recorder` already handled abrupt publisher disconnect** — `finalize()`
   is idempotent and produces a valid FLV even with no media. Verified by new
   tests against the real async sink; not changed.
4. **`RecorderSink` was already correctly wired into `CommandSession`** for
   audio/video/metadata and both disconnect paths. No change was needed to
   `CommandSession` at all.
5. **The FLV byte layer was already correct**, including 24-bit-plus-extended
   timestamps and the `parse_flv` read-back parser. Reused as-is.

---

## 4. Architecture decisions

**Portable async sink rather than a synchronous one.** `AsyncFileSink` owns a
dedicated writer thread and a bounded FIFO; `append`/`patch`/`finalize` only
touch memory and return. No public method performs disk I/O on the calling
thread. Full role/defect/change/risk justification per `v2_promot.md` 3.2 is
in `docs/recording.md`.

**`finalize()` is non-blocking.** It is called from the RTMP thread on
unpublish, so it only sets a flag; the writer drains, `fsync`s, closes and
renames. `wait_for_completion(timeout)` exists for shutdown and tests.

**Temp file + atomic rename.** Writes go to `<path>.part`; only a fully
written recording ever appears at the final path. A failed recording is
deliberately left as `.part` rather than published.

**Queue overflow is backpressure, not corruption.** The sink returns
`ResourceExhausted`, distinct from `StorageWriteFailed`; `Recorder` counts a
dropped frame and keeps recording. This is the one behavioural change to
`Recorder`.

**MPEG-TS, not CMAF/fMP4.** Chosen for segment independence (each segment
carries its own PAT/PMT, no init segment to coordinate) and reconnect
robustness — the two hardest correctness problems in a live packager — at a
cost of ~4% framing overhead. Full comparison table in `docs/hls.md`. The
consequence, recorded honestly: **LL-HLS is not implemented**, since partial
segments really want CMAF.

**Segments held in memory, not on disk.** Keeps disk I/O entirely off the
HTTP request path and avoids write-then-read amplification. Bytes live in a
`core::SharedBuffer` (`shared_ptr<const vector<byte>>`) — the same pattern
`live_fanout` uses — so a segment is **never deep-copied per viewer** (3.8).

**Bounded cleanup with a grace window.** `live_window_segments` are
advertised; `retention_grace_segments` more are retained but unadvertised, so
a player holding a slightly stale playlist gets a 200 rather than a burst of
404s. Plus an absolute `max_total_bytes` cap.

**`TeeRecorderSink` instead of widening `CommandSession`.** `CommandSession`
holds a single `RecorderSink*`; Phase 6 adds a second consumer. Composing
them behind the existing interface leaves the hot media path and
`CommandSession` completely untouched.

**HLS served by the existing Phase 5 `HttpServer`,** as an explicitly
chainable handler placed in front of the management handler. One bounded HTTP
server serves both; no second HTTP stack, and Phase 5's bounded-connection
posture is preserved.

**`plan_retention` is a pure function** over a file list, separate from
`apply_retention`'s filesystem work — the same testability seam as
`Recorder`/`FileSink`.

---

## 5. Files added

**Recording**
- `include/rtmp_server/recording/async_file_sink.hpp`
- `src/recording/async_file_sink.cpp`
- `include/rtmp_server/recording/retention.hpp`
- `src/recording/retention.cpp`

**Codec helpers (HLS passthrough)**
- `include/rtmp_server/media/h264/avc.hpp`, `src/media/h264/avc.cpp`
- `include/rtmp_server/media/aac/adts.hpp`, `src/media/aac/adts.cpp`
- `include/rtmp_server/media/ts/ts_muxer.hpp`, `src/media/ts/ts_muxer.cpp`

**HLS**
- `include/rtmp_server/hls/segment.hpp`
- `include/rtmp_server/hls/playlist.hpp`, `src/hls/playlist.cpp`
- `include/rtmp_server/hls/segmenter.hpp`, `src/hls/segmenter.cpp`
- `include/rtmp_server/hls/segment_store.hpp`, `src/hls/segment_store.cpp`
- `src/hls/CMakeLists.txt`

**Delivery / composition**
- `include/rtmp_server/control/hls_http_handler.hpp`, `src/control/hls_http_handler.cpp`
- `include/rtmp_server/protocol/commands/tee_recorder_sink.hpp`

**Tests**
- `tests/recording/async_file_sink_test.cpp`, `tests/recording/retention_test.cpp`
- `tests/hls/CMakeLists.txt`, `tests/hls/test_media.hpp`
- `tests/hls/codec_test.cpp`, `ts_muxer_test.cpp`, `playlist_test.cpp`,
  `segmenter_test.cpp`, `segment_store_test.cpp`, `hls_http_test.cpp`

**Docs**
- `docs/hls.md`, `docs/recording.md`, `docs/phase-6-report.md`

---

## 6. Files modified

| File | Change |
|---|---|
| `CMakeLists.txt` | Added `src/hls` (before `src/control`, which links it) and `tests/hls`. |
| `src/media/CMakeLists.txt` | Added h264/aac/ts sources and the two new recording sources. |
| `src/control/CMakeLists.txt` | Added `hls_http_handler.cpp`; linked `rtmp_server_hls`. |
| `tests/recording/CMakeLists.txt` | Added the two new test files. |
| `src/recording/recorder.cpp` | `write_tag` treats sink `ResourceExhausted` as a counted drop rather than a fatal failure. |
| `src/control/http_server.cpp` | Proper HTTP reason phrases; skip the built-in `Content-Length` when a handler set one (avoids a duplicate header on `HEAD`). |

No file was deleted and no existing behaviour was removed.

---

## 7. Public interfaces changed

**Added** (all new, no existing signature altered):
`recording::AsyncFileSink` (+ `Options`, `Stats`), `recording::RetentionPolicy`
/ `RecordingFile` / `RetentionPlan` / `plan_retention` / `apply_retention`,
`media::h264::*`, `media::aac::*`, `media::ts::TsMuxer` / `mpeg_crc32`,
`hls::Segment` / `SegmentPtr` / `MediaPlaylistOptions` / `Rendition` /
`build_media_playlist` / `build_master_playlist` / `Segmenter` /
`SegmentStore`, `control::HlsHttpHandler` (+ `HlsHttpOptions`, content-type
constants), `protocol::commands::TeeRecorderSink`.

**Behaviourally changed, signature identical:**
`recording::Recorder::on_audio/on_video` — a sink returning
`ResourceExhausted` now increments `dropped_frames` instead of setting
`stats().failed`. Strictly more forgiving; no caller change required.

**`control::HttpServer`** — no interface change. `HttpResponse` already
carried a `headers` map; the server now honours a handler-supplied
`Content-Length` and emits correct reason phrases.

---

## 8. Tests added

**114 new tests**, all executed.

`tests/recording/async_file_sink_test.cpp` (15):
atomic `.part`→rename; patch ordering after queued appends; open failure;
queue overflow rejected explicitly and counted; Recorder-level overflow keeps
recording and still yields valid FLV; disk-space exhaustion marks unhealthy
and refuses to publish; append-after-failure returns the error; RAII finalize
via destructor; abrupt publisher disconnect mid-recording produces valid FLV;
finalize idempotence; **writes happen on the sink writer thread, not the
caller**; **command-path and finalize latency bounded while the disk works**;
stale `.part` from a crash is neither served nor appended to.

`tests/recording/retention_test.cpp` (7): each limit alone, all combined
without double-counting, on-disk application ignoring `.txt` and `.part`,
missing directory error.

`tests/hls/codec_test.cpp` (13): FLV video/audio header parsing, signed CTS
sign-extension, non-AVC/non-AAC rejection, decoder-config parsing, length
overrun rejection (config and sample), AVCC→Annex B with/without parameter
sets, ASC parsing incl. reserved index rejection, bit-exact ADTS header
validation.

`tests/hls/ts_muxer_test.cpp` (12): MPEG CRC-32 against the standard
`"123456789"` check value; PAT/PMT framing; **PSI section CRCs verified**;
PMT stream types 0x1B/0x0F; every packet exactly 188 bytes; keyframe PCR +
`random_access_indicator`; non-keyframes carry neither; continuity counters
increment per PID **and wrap correctly at 16**; separate audio/video PIDs;
PES PTS/DTS decoded back and compared; audio PES PTS-only; empty AU
rejection; `reset_continuity`.

`tests/hls/playlist_test.cpp` (14): a strict **RFC 8216 validator** (see §11),
a negative test proving the validator catches known-bad playlists,
TARGETDURATION auto-raise, MEDIA-SEQUENCE, discontinuity tag ordering,
DISCONTINUITY-SEQUENCE, ENDLIST placement, URI prefixes, 3-decimal EXTINF,
empty list, null-pointer safety, master playlist ordering/attributes.

`tests/hls/segmenter_test.cpp` (18): keyframe-aligned cutting, PAT/PMT at
every segment head, sequential naming, media-before-config dropped,
audio-before-keyframe dropped, video **and** audio sequence-header changes,
identical header is *not* a change, malformed header rejected without state
loss, backward/forward timestamp discontinuity, small jitter is not,
publisher reconnect, byte and duration force-cuts, forced cuts flagged
discontinuous, finalize publishes the tail, finalize idempotence, codecs
attribute, **shared buffers not copied per consumer**.

`tests/hls/segment_store_test.cpp` (12): retrieval, **bounded retention =
window + grace**, byte cap, always-keep-one, live window vs. fetchable grace,
media sequence advance, discontinuity-sequence accounting, ENDLIST, clear,
null safety, evicted segment stays valid for a holder, **concurrent producer
+ 6 reader threads**.

`tests/hls/hls_http_test.cpp` (23): content types for playlists/segments,
master playlist, 404s, **non-cacheable 404**, 405 + Allow, path traversal,
handler chaining to the management API, prefix boundary (`/hlsx` not
swallowed), **Cache-Control per resource**, CORS/Accept-Ranges, HEAD with
accurate Content-Length and no body, Range 206 + Content-Range, open-ended
and suffix ranges, 416 for unsatisfiable and malformed, **token required /
valid / expired / forged / wrong-stream**, non-cacheable 403, **token query
propagated onto segment URIs**, token disabled, unregister, **8 concurrent
viewer threads against a live producer**.

---

## 9. Commands executed

```bash
git log --oneline -5
which ffmpeg ffprobe cmake
grep -ril "hls\|m3u8" include/ src/ tests/
grep -rn "FileSink" --include='*.hpp' --include='*.cpp' --include='*.txt' .

cmake --preset core-only -DRTMP_SERVER_CORE_ONLY=ON
cmake --build --preset core-only
ctest --preset core-only

cmake --preset asan -DRTMP_SERVER_CORE_ONLY=ON
cmake --build --preset asan
ctest --preset asan

./build/core-only/tests/recording/rtmp_server_recording_tests
./build/core-only/tests/hls/rtmp_server_hls_tests
```

---

## 10. Actual build result

**core-only** — configure and build succeed. Zero compiler errors, zero
compiler warnings (the project builds with its `CompilerWarnings` module
active, including `-Wsign-conversion`, which caught and required a fix in
`ts_muxer.cpp`). The only linker output is the repository's pre-existing
benign `ld: warning: ignoring duplicate libraries` notes.

**asan** (`RTMP_SERVER_CORE_ONLY=ON`, `RTMP_SERVER_ENABLE_ASAN=ON`) — configure
and build succeed, no errors or warnings.

---

## 11. Actual test result

| Preset | Result |
|---|---|
| Baseline (before this phase) | **262 / 262 passed** |
| `core-only` (after) | **376 / 376 passed** — 100%, 4.14 s |
| `asan` (after) | **376 / 376 passed** — 100%, 19.14 s |

376 = 262 baseline + 114 added. **No pre-existing test was modified, skipped
or disabled**, and no test regressed.

Per-binary: `rtmp_server_recording_tests` 28/28 (13 pre-existing + 15 new);
`rtmp_server_hls_tests` 94/94 (all new).

### Playlist validation — and the ffmpeg caveat

`which ffmpeg ffprobe` reports **neither is installed on this host**. The
definition of done asks for validation "with a real player or validation
tool"; that was **not possible here**, and no such validation is claimed.

The substitute, as instructed, is a rigorous structural check:

- `tests/hls/playlist_test.cpp` contains a hand-written **RFC 8216
  validator** that parses generated playlists and enforces real tag rules:
  `#EXTM3U` first line; exactly one `EXT-X-TARGETDURATION` and it must be
  present and non-zero; `EXTINF` must carry its terminating comma; every
  `EXTINF` must be followed by a URI and every URI preceded by one; no
  consecutive `EXT-X-DISCONTINUITY`; no URI after `EXT-X-ENDLIST`; and
  **every segment duration rounded to the nearest integer must be ≤
  TARGETDURATION** (4.3.3.1). A dedicated test feeds it four known-bad
  playlists to prove the validator actually rejects violations rather than
  rubber-stamping.
- `tests/hls/ts_muxer_test.cpp` re-derives MPEG-TS framing from the emitted
  bytes rather than trusting the muxer: sync-byte alignment, exact 188-byte
  packets, per-PID continuity-counter monotonicity across a wrap, PES start
  codes, PTS/DTS decoded back and compared to the inputs, and **PSI section
  CRC-32s recomputed and verified** (with the CRC itself checked against the
  standard `"123456789"` = `0x0376E6E7` vector).

That is stronger than a substring check but is **not** equivalent to a real
player decoding the output. See §14.

---

## 12. Sanitizer result

`ctest --preset asan` with `RTMP_SERVER_CORE_ONLY=ON`:
**376/376 passed, zero findings.** No AddressSanitizer error, no
LeakSanitizer report, no UBSan runtime error
(`grep -icE "ERROR: (Address|Leak)Sanitizer|runtime error|SUMMARY:"` over
both the build and test logs returns 0).

This covers the multithreaded paths specifically: the `AsyncFileSink`
producer/writer handoff, the `SegmentStore` concurrent producer + 6 readers
test, and the `HlsHttpHandler` 8-concurrent-viewer test all ran clean under
ASan.

**TSan was not run.** The `tsan` preset exists but was not part of the
requested command set. Given the new threading (a writer thread per
recording, plus concurrent segment store access), running it is the single
highest-value follow-up — noted in §14 and §18.

---

## 13. Performance observations

Measured incidentally, not a benchmark — no load test was run in this phase.

- Full `core-only` suite: 4.14 s for 376 tests. ASan: 19.14 s (~4.6×, normal).
- The HLS suite (94 tests, including TS muxing, segmentation and two
  multithreaded stress tests) runs in **55 ms**, so packaging cost per frame
  is negligible relative to network work.
- `NoDiskIoOnMediaThreadTest.CommandPathCallsReturnPromptlyWhileTheDiskIsBusy`
  drives ~8 MB through the recorder and asserts worst-case single-call
  latency and `finalize()` latency both stay under 1 s; actual observed
  values are orders of magnitude below that, consistent with the calls doing
  only a memcpy into a queue.
- MPEG-TS framing overhead is ~4% (188-byte packets with adaptation-field
  stuffing), the expected and accepted cost of the container choice.
- Memory per stream is bounded by construction: recording ≤ 32 MiB queued,
  HLS ≤ `max_total_bytes` (256 MiB default) + one in-progress segment
  (≤ 16 MiB default).

---

## 14. Remaining risks

1. **No real-player validation.** ffmpeg/ffprobe are absent from this host, so
   no decoder has ever consumed the generated TS. The structural checks are
   thorough but cannot prove a real player renders it. **This is the single
   biggest risk in the phase.** Mitigation: run `ffprobe`/`hls.js`/Safari
   against a generated segment set on a machine that has the tooling, before
   any production use.
2. **TSan not run** against the new threading. ASan is clean, but ASan is not
   a race detector.
3. **The HLS path is not yet wired into a running server.** `Segmenter`,
   `SegmentStore` and `HlsHttpHandler` are complete, unit-tested and
   composable via `TeeRecorderSink`, but nothing in `apps/rtmp_server`
   constructs them — exactly as `Recorder` was before this phase, and for the
   same reason: the server executable is Linux/io_uring-gated and cannot be
   built or run on this host. Wiring is a small, mechanical change that
   belongs in a phase where it can actually be executed and tested end to end.
4. **No end-to-end test with a real RTMP publisher.** All media in the HLS
   tests is synthetic (hand-built FLV payloads). Real encoder output (B-frames,
   varied GOP structures, SEI, multi-slice pictures) has not been exercised.
5. **B-frame / composition-time handling is implemented but lightly tested.**
   CTS sign-extension has a unit test, but no test drives a realistic B-frame
   reordering pattern.
6. **`AudioSpecificConfig` sampling-frequency index 15** (explicit rate) is
   rejected rather than supported. No mainstream RTMP encoder emits it, but a
   publisher that did would produce no HLS audio.
7. **Segments do not survive a process restart** (in-memory only). Correct for
   live, but HLS cannot serve VOD of a past stream.
8. **Retention is not scheduled.** `apply_retention()` is implemented and
   tested but nothing calls it periodically; a maintenance thread is needed.
9. **Disk-space monitoring is per-sink and reactive** — it aborts the
   recording that notices the shortage rather than proactively refusing new
   recordings server-wide.

---

## 15. Breaking changes

**None.**

- No public signature was removed or altered.
- `CommandSession`, `RecorderSink`, `FileSink`, `IoUringFileSink` and the FLV
  writer are unchanged.
- The one behavioural change (`Recorder` treating `ResourceExhausted` as a
  drop) is strictly more forgiving and cannot break a caller — the existing
  `IoUringFileSink` never returns that code, so its behaviour is bit-identical.
- The `http_server.cpp` changes are additive: reason phrases only improve the
  status line, and the `Content-Length` change only takes effect when a
  handler explicitly sets that header, which no pre-existing handler does.
- All 262 pre-existing tests pass unmodified.

---

## 16. Rollback considerations

Rollback is low-risk and can be partial.

- **Full rollback:** revert the commit. Every added file is new; the six
  modified files have small, isolated diffs.
- **Drop HLS only:** remove `add_subdirectory(src/hls)` and `tests/hls` from
  the root `CMakeLists.txt`, drop `hls_http_handler.cpp` and the
  `rtmp_server_hls` link from `src/control/CMakeLists.txt`. The recording
  improvements are independent and would still build.
- **Drop the recording changes only:** revert `src/recording/recorder.cpp`
  and remove the two new sources from `src/media/CMakeLists.txt`. HLS does
  not depend on `AsyncFileSink`.
- **No data migration, no schema change, no config change** is involved.
  Nothing in this phase alters on-disk state written by earlier phases, and
  since nothing yet constructs these components at runtime, reverting cannot
  affect a running deployment's behaviour.
- `.part` files are the only new on-disk artifact; they are inert.

---

## 17. Definition-of-done checklist

| Requirement | Status | Evidence |
|---|---|---|
| Recording cannot block media workers | **Met** | `AsyncFileSink`: no public method does disk I/O on the caller. Proven by `EveryWriteHappensOnTheSinkWriterThreadNotTheCaller` + `CommandPathCallsReturnPromptlyWhileTheDiskIsBusy` (executed). |
| Recording memory usage is bounded | **Met** | Two independent caps (Recorder soft, sink hard). `QueueOverflowIsRejectedExplicitlyAndCounted` asserts `pending_bytes() <= max_queue_bytes`. |
| Recording failure policy explicit, no silent drops | **Met** | Every mode counted/logged; `ResourceExhausted` vs `StorageWriteFailed` distinguished. Table in `docs/recording.md`. |
| Safe rotation + atomic finalize | **Met** | `.part` + `rename`; `WritesThroughTemporaryFileAndAtomicallyRenames`, `LeftoverPartFileFromACrashIsNotMistakenForARecording`. |
| Disk-space monitoring | **Met** | `statvfs` on the writer thread; `DiskSpaceExhaustionMarksSinkUnhealthyAndSkipsPublish`. |
| Configurable retention | **Met** | Age/count/size, `retention_test.cpp` (7 tests). |
| H.264/AAC passthrough produces valid HLS | **Met structurally** | TS framing, PSI CRCs, PES timestamps all verified byte-level. **Not** validated by a real decoder — see §14.1. |
| Playlists validated with a real player or tool | **Substituted** | ffmpeg/ffprobe absent (verified). Rigorous RFC 8216 validator + self-test used instead, per instruction. Honestly not equivalent. |
| Master playlist for multiple renditions | **Met** | `build_master_playlist`, ordering + attribute tests. |
| Configurable segment duration and live window | **Met** | `SegmenterConfig::target_duration`, `SegmentStoreConfig::live_window_segments`. |
| EXT-X-DISCONTINUITY handling | **Met** | Codec change, rollover, forward gap, reconnect — all tested. |
| Atomic playlist updates | **Met** | Playlists are rendered on demand from an immutable snapshot taken under lock; a reader never sees a half-updated playlist. |
| Bounded segment cleanup | **Met** | window + grace + byte cap; `RetainedSegmentCountIsBoundedByWindowPlusGrace`. |
| Correct Cache-Control per playlist vs segment | **Met** | `LivePlaylistIsNotCacheableButSegmentsAreImmutable`, plus no-store on 404/403. |
| Signed playback token integration | **Met** | Same `management::verify_token` as RTMP; 5 auth tests incl. forged/expired/wrong-stream. |
| Served via existing `control::HttpServer` | **Met** | Chainable handler; `RequestsOutsideThePrefixAreForwardedToTheNextHandler`. |
| CDN-compatible HTTP behaviour documented | **Met** | `docs/hls.md` — content types, Cache-Control table, Range, CORS. |
| No deep media copies per viewer | **Met** | `core::SharedBuffer`; `SegmentBytesAreSharedNotCopiedPerConsumer` asserts identical buffer address across 100 holders. |
| Every client-influenced resource bounded | **Met** | Segment bytes/duration, store count/bytes, recording queue, AMF/parse lengths validated. |
| No callbacks under locks | **Met** | `SegmentStore` renders outside the lock; `HlsHttpHandler` copies the entry then releases; `Segmenter` invokes its callback with no lock held. |
| Required tests (all 12 categories) | **Met** | §8 maps every one. |
| Build + tests green, core-only and asan | **Met** | 376/376 both; §11, §12. |
| No TODO placeholders / no fake completion | **Met** | No `TODO` introduced; every claim above traces to an executed test. |

**Not done (explicit):** LL-HLS; real-player validation; runtime wiring into
the server executable; TSan run. Each is explained in §14 with a reason.

---

## 18. Recommended next phase

The prompt's PHASE 7 is *Observability, real load testing and capacity
validation*, which remains the right next step. Before or alongside it, three
Phase 6 loose ends should be closed — all of them blocked on this host rather
than on design:

1. **Validate against a real decoder** on a machine with ffmpeg/ffprobe:
   `ffprobe` a generated segment set and play the playlist in hls.js/Safari.
   This closes the largest remaining risk (§14.1).
2. **Run the `tsan` preset** against the new writer-thread and segment-store
   concurrency.
3. **Wire `Segmenter`/`SegmentStore`/`HlsHttpHandler` and `AsyncFileSink` into
   `apps/rtmp_server`** via `TeeRecorderSink`, plus a maintenance thread
   calling `apply_retention()`. Best done on Linux where the server actually
   builds and can be exercised end to end with a real encoder.

Phase 7 should then add HLS- and recording-specific metrics to the metric set
it defines — `hls_segments_produced`, `hls_segment_requests`,
`hls_discontinuities`, `recording_dropped_frames`, `recording_queue_bytes`,
`recording_failures` — since the counters (`SegmenterStats`,
`SegmentStoreStats`, `HlsHttpHandler::Stats`, `RecorderStats`,
`AsyncFileSink::Stats`) are already implemented and populated, and only need
exporting.
