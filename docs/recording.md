# Recording (Phase 6 state)

This document describes the recording path **as of Phase 6**. The FLV
container format and the original Recorder design are documented in
`docs/flv-recording.md`; this file covers what Phase 6 changed and why.

## What Phase 6 changed, and why

### The defect that was actually found

`recording::Recorder` was already implemented, bounded and unit-tested, and
already wired into `CommandSession` via the `RecorderSink` hook. The audit
question was whether disk I/O could block the RTMP thread.

The answer was more specific than "yes" or "no":

> The **only** concrete `FileSink` implementation in the tree was
> `io::io_uring::IoUringFileSink`, compiled exclusively under
> `NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux"`.
> On any core-only or non-Linux build there was **no production sink at
> all** — the abstract interface had exactly one implementation outside of
> the test fake. Additionally, nothing in the server ever constructed a
> `Recorder` or a sink; the recording library was complete but unwired.

So the io_uring path was genuinely non-blocking on Linux, and the portable
build simply could not record. The risk was not an existing blocking write —
it was that the obvious fix (a plain `::write()` sink) *would* have put
synchronous disk I/O directly on the RTMP command-processing thread.

Per `docs/v2_promot.md` 3.2, before changing anything:

| | |
|---|---|
| **Role** | `FileSink` is the byte sink `Recorder` writes an FLV file through, deliberately decoupled from the I/O mechanism. |
| **Defect** | Only a Linux/io_uring implementation existed; the portable build had no writer, and a naive one would block the media thread. |
| **Change** | Add `recording::AsyncFileSink`: a portable POSIX sink with a dedicated writer thread and a bounded queue. Add `recording::retention`. Make `Recorder` treat sink `ResourceExhausted` as backpressure rather than a fatal error. |
| **Risk** | A second sink implementation to keep in step with the `FileSink` contract. Mitigated by both being driven through the same `Recorder` tests, and by the portable one being what the portable build actually uses. |

`Recorder`, `FileSink`, `IoUringFileSink` and the FLV writer are otherwise
**unchanged**; no working functionality was removed.

## AsyncFileSink

```text
RTMP media thread                    writer thread (owned by the sink)
─────────────────                    ────────────────────────────────
append(bytes) ──► [bounded FIFO] ──► pwrite / fsync / rename
patch(off,b)  ──►      ▲
finalize()    ──► flag ┘             (statvfs disk-space monitor)
```

**No public method performs disk I/O on the calling thread.** `append`,
`patch` and `finalize` only touch an in-memory queue and return. This is the
central Phase 6 constraint (`docs/v2_promot.md` 3.6) and it is asserted by an
executed test, not argued in prose — `AsyncFileSink::writer_thread_id()` is
compared against the media thread's id in
`tests/recording/async_file_sink_test.cpp`.

### Ordering

Appends and patches share one FIFO, so a patch can never overtake the appends
it is meant to overwrite. Writes use `pwrite` so a patch never disturbs the
append cursor.

### Atomic finalize

Bytes are written to `<path>.part`. On `finalize()` the writer drains the
queue, `fsync()`s, closes, and `rename()`s the temp file into place.
Consequences:

- A reader watching the output directory **never** observes a partial file
  under the final name.
- A crash mid-recording leaves an obvious `.part` artifact rather than a
  truncated file that looks finished.
- On the failure path the `.part` file is deliberately **left behind** as
  evidence and is *not* published under the final name.

`finalize()` is non-blocking because it is called from the RTMP thread on
unpublish/disconnect. Callers that must observe the outcome (graceful
shutdown, tests) use `wait_for_completion(timeout)`. The destructor always
joins the writer thread and closes the fd (RAII; no detached threads).

### Failure policy

Every failure mode is explicit and counted — there are no silent drops.

| Condition | Behaviour |
|---|---|
| Queue full | `append` returns `ResourceExhausted`; `Recorder` counts a dropped frame and **keeps recording** (backpressure, not corruption) |
| `pwrite` fails | Sink marked unhealthy with `StorageWriteFailed`; recording aborts; file not published |
| Short write | Not an error — the writer advances and continues |
| `EINTR` | Retried |
| Free space below `min_free_bytes` | Sink marked unhealthy with `StorageUnavailable`; recording aborts; file not published |
| `statvfs` fails | Logged as a warning; recording continues (an unavailable stat must not kill a working recording) |
| Append after failure | Returns the recorded error |
| Append after finalize | `InvalidStateTransition` |

The distinction between `ResourceExhausted` (drop this frame) and everything
else (stop recording) is the one behavioural change made to `Recorder`.

### Bounded memory

Two independent bounds:

- `RecorderConfig::max_queued_bytes` (default 16 MiB) — the Recorder's soft
  policy, checked against `pending_bytes()` before framing a tag.
- `AsyncFileSink::Options::max_queue_bytes` (default 32 MiB) — the sink's own
  hard cap, enforced inside `append`.

A permanently stalled disk therefore costs bounded memory and dropped frames,
never unbounded growth.

### Disk-space monitoring

The writer thread calls `statvfs` every `disk_check_interval_bytes` (default
8 MiB) of written data — on the writer thread, because `statvfs` is a
blocking syscall and must not run on the producer. Below `min_free_bytes`
(default 64 MiB) the sink fails cleanly rather than filling the filesystem.

## Retention

`recording::RetentionPolicy` supports three independent limits, any of which
may be disabled by setting it to zero:

- `max_age`
- `max_files`
- `max_total_bytes`

Limits combine; the **oldest** recordings are always evicted first.

`plan_retention()` is a pure decision function over a list of files — no
filesystem access — so the policy is exhaustively unit-testable with no disk.
`apply_retention()` does the directory scan and the unlinking.

**`apply_retention()` performs blocking filesystem I/O and must not be called
from an RTMP event-loop thread** — run it from a maintenance thread or a
management-API worker. In-progress `.part` files are never considered, so a
live recording can never be deleted out from under its writer, and a crash
artifact is never mistaken for a finished recording.

## Concurrent recording and HLS

`protocol::commands::TeeRecorderSink` fans `CommandSession`'s single
`RecorderSink*` hook out to several sinks, so FLV recording and HLS packaging
consume the same media without widening `CommandSession`'s interface.
`CommandSession` itself is unchanged. Order is preserved — recording runs
first — so a slow or failing HLS sink can never prevent a recording from
being written.

## Testing

`tests/recording/async_file_sink_test.cpp` and `retention_test.cpp` cover:
start/stop, atomic `.part` → rename, patch ordering, open failure, queue
overflow (both at the sink and through the Recorder), disk-space exhaustion,
append-after-failure, RAII finalize without an explicit call, abrupt
publisher disconnect mid-recording, finalize idempotence, the writer-thread
boundary proof, command-path latency, process-restart/stale-`.part`
behaviour, and every retention limit alone and combined.

Recordings written by these tests are parsed back with
`media::flv::parse_flv` to confirm they are structurally valid FLV.
