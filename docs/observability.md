# Observability — metrics and structured logging

Phase 7 (`docs/v2_promot.md` PHASE 7). This document describes what the
server measures, how it is exposed, what it deliberately does **not** record,
and where each metric is actually incremented in the code.

---

## 1. Design

Two independent subsystems, both living in `rtmp_server_core` (so every other
library can depend on them without a cycle):

| Component | Header | Source |
|---|---|---|
| Metric registry | `include/rtmp_server/observability/metrics.hpp` | `src/observability/metrics.cpp` |
| Structured logger | `include/rtmp_server/observability/logger.hpp` | `src/observability/logger.cpp` |
| Audit log (Phase 5) | `include/rtmp_server/observability/audit_log.hpp` | `src/observability/audit_log.cpp` |

Both are **non-owning dependencies** at every call site: a component takes a
`Metrics*` via `set_metrics()`, defaulting to `nullptr`. When null, every
metric call site is a single predictable branch and behaviour is byte-for-byte
what it was before Phase 7. This is why all pre-existing tests continued to
pass unmodified.

---

## 2. Metric registry

### 2.1 Two access paths

**Declared catalog (hot-path).** Every Phase 7 metric is a compile-time
`MetricId` enumerator generated from the `RTMP_SERVER_METRIC_TABLE` X-macro.
Storage is one fixed `std::array<std::atomic<std::int64_t>, kCount>`.
Increments are lock-free, allocation-free, and involve no string hashing, so
they are safe on the per-packet media path and inside io_uring completion
handlers.

```cpp
metrics.increment(MetricId::DroppedVideoFrames);       // counter
metrics.add(MetricId::ActiveViewers, +1);              // gauge, signed delta
metrics.set(MetricId::GopCacheBytes, total);           // gauge, absolute
```

**Dynamic path (cold).** `increment_counter(std::string_view)` /
`set_gauge(...)` remain for Phase 5's `StreamManager`. Mutex-guarded,
map-backed, explicitly *not* for the hot path.

### 2.2 Cardinality policy

> *"Avoid high-cardinality metric labels such as raw connection IDs."*

This is enforced mechanically, not just by convention:

1. **The catalog is closed.** Names are fixed at compile time; no runtime
   input can add a series.
2. **`is_valid_dynamic_name()` rejects identifier-shaped names.** A name must
   match `[a-z][a-z0-9_:]*`, be ≤96 chars, and contain **no run of 4+
   digits** — the signature of an interpolated connection ID, stream ID, port
   or timestamp. Rejected writes increment `metrics_rejected_names_total`
   rather than being silently dropped.
3. **The dynamic map is bounded** at `kMaxDynamicMetrics` (256). Beyond that,
   new names are refused, not allocated.
4. **No metric is ever labelled by** connection ID, stream ID, stream key,
   subscriber ID, client IP, or user agent.

Two required metrics needed a design decision because a naive implementation
would be unbounded:

| Required name | How it is exposed | Why |
|---|---|---|
| `viewers_per_stream` | `viewers_per_stream_max`, `viewers_per_stream_mean_milli`, `active_streams` | Stream count is unbounded and stream keys are publish secrets. `LiveFanout::sample_gauges()` feeds one sample per live stream via `observe_viewers_per_stream()`, then `commit_viewers_per_stream()` publishes the aggregate. |
| `connections_per_worker` | `connections_per_worker{worker="N"}` | Worker count is bounded by *configuration* (`kMaxWorkers` = 64), never by client behaviour, so a label is safe here. Only populated slots are exported. |

This behaviour is asserted in
`tests/unit/observability/metrics_test.cpp`
(`MetricsCardinalityTest.DynamicNamesCarryingAnIdentifierAreRejected`,
`TheDynamicRegistryIsBounded`, `MetricsWorkerTest`,
`MetricsAggregationTest`).

### 2.3 Derived series

`ingress_bitrate` / `egress_bitrate` are not accumulated per packet. They are
computed by `refresh_derived()` from the byte counters and the wall time since
the previous call — two samples are required, so the first call only
establishes a baseline and reports 0. `process_memory_bytes` is sampled by
`refresh_process_metrics()` (mach `task_info` on Darwin, `/proc/self/statm` on
Linux). Both are called from the `/metrics` handler and from the test server's
periodic sampler — **never** from a network worker.

### 2.4 Where each metric is incremented

| Metric | Call site | Built in core-only? |
|---|---|---|
| `active_connections` | `RtmpAuthenticator::admit_connection` / `release_connection`; `IoUringEventLoop` accept/close | authenticator ✅ / event loop ❌ |
| `active_publishers`, `publisher_disconnects` | `StreamRegistry::register_publisher` / `unregister_publisher` / `unregister_all_for_connection` | ✅ |
| `active_viewers`, `viewer_disconnects` | `LiveFanout::subscribe` / `unsubscribe` / eviction / `publisher_stopped` | ✅ |
| `viewers_per_stream_*`, `active_streams`, `gop_cache_*`, `outbound_queue_*` | `LiveFanout::sample_gauges()` (periodic sampler) | ✅ |
| `ingress_bytes_total` | `RtmpConnectionSession::on_bytes_received` | ✅ |
| `egress_bytes_total` | `LiveFanout::run_deliveries` (batched per frame, not per viewer); `IoUringEventLoop::handle_send_completion` | fan-out ✅ / loop ❌ |
| `dropped_video_frames`, `dropped_audio_frames` | `LiveFanout::dispatch_locked`, `ViewerQueue::Decision::DropAndWait` | ✅ |
| `slow_viewer_recoveries` | `Decision::DeliverResumed` | ✅ |
| `slow_viewer_evictions` | `Decision::Evict` | ✅ |
| `authentication_failures` | `RtmpAuthenticator::record_auth_result_locked` | ✅ |
| `recording_queue_depth`, `recording_failures` | `AsyncFileSink::append` / `patch` / `mark_failed` / writer drain | ✅ |
| `partial_send_count` | `TcpConnection::on_send_completion` → `IoUringEventLoop::handle_send_completion` | ❌ Linux only |
| `connection_timeouts` | `IoUringEventLoop` idle/handshake timeout completions | ❌ Linux only |
| `io_uring_sq_full` | every `sqe_exhausted_*` site in `IoUringEventLoop` | ❌ Linux only |
| `provided_buffer_exhaustion` | `IoUringEventLoop::submit_receive` pool-exhausted branch | ❌ Linux only |
| `io_uring_cq_overflow` | **registered, not yet fed** — see §2.5 | ❌ |
| `inter_worker_queue_depth`, `inter_worker_queue_drops` | **registered, not yet fed** — see §2.5 | ❌ |
| `worker_cpu_usage` | **registered, not yet fed** — see §2.5 | ❌ |
| `process_memory_bytes` | `Metrics::refresh_process_metrics()` | ✅ |

### 2.5 Honestly declared gaps

These metrics exist in the catalog and are exported (as `0`) but **nothing
increments them yet**:

* `io_uring_cq_overflow` — requires reading `IORING_SQ_CQ_OVERFLOW` from the
  ring's flags after `io_uring_submit_and_wait`. Not wired.
* `inter_worker_queue_depth` / `inter_worker_queue_drops` — `CrossWorkerRouter`
  (Phase 4) has the queue but does not yet report depth or drops.
* `worker_cpu_usage` — needs per-thread `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`
  sampling in `WorkerPool`.

They are declared rather than omitted so the exposition surface is stable and
a dashboard can be built against the complete set; a reader must treat a `0`
here as "not instrumented", not "nothing happened".

### 2.6 Exposition

`Metrics::render_prometheus()` emits Prometheus text format v0.0.4 with
`# HELP` and `# TYPE` for every catalog entry. Served by
`ManagementApi::handle_metrics()` at `GET /metrics`.

Phase 7 fixed a real defect here: Phase 5's `StreamManager` emitted
`management.<action>_total`. A `.` is **not legal** in a Prometheus metric
name, so those series were unscrapeable despite the endpoint advertising
`text/plain; version=0.0.4`. They are now `management_<action>_total`, and
`is_valid_dynamic_name()` prevents the class of bug from recurring.

---

## 3. Structured logging

### 3.1 Format

One JSON object per line (JSON-lines) on stdout (`< Warn`) or stderr
(`>= Warn`). Every record carries `timestamp` (ISO-8601, ms, UTC), `level`,
`component`, `event`.

`LogContext` supplies the Phase 7 field set; only the members that are set are
emitted, so a handshake-stage record does not carry an empty `stream_id`:

| Field | Type |
|---|---|
| `worker_id` | `std::uint32_t` |
| `connection_id` | `std::uint64_t` |
| `stream_id` | `std::uint64_t` (the server-minted `StreamId`, never the stream key) |
| `application_id` | `std::uint64_t` |
| `error_code` | `std::int32_t` |
| `latency_us` | microseconds |
| `request_id` | string |

```cpp
LogContext ctx;
ctx.with_worker(2).with_connection(id).with_stream(sid)
   .with_latency(std::chrono::microseconds{842});

Logger::instance().log(LogLevel::Info, "event_loop", "viewer_attached", ctx,
                       {LogField{"transport", "rtmp"}});
```

### 3.2 JSON correctness

Phase 7 fixed a second real defect: the pre-Phase-7 logger interpolated field
keys and values **raw** into the JSON line. Any value containing `"`, `\`, or
a newline produced a malformed record — and a value could inject synthetic
fields into a downstream log pipeline (e.g. a fake `"level":"error"`).
`escape_json_into()` now escapes quotes, backslashes, `\n\r\t\b\f`, and all
remaining C0 controls as `\u00XX`. Asserted by `LoggerJsonTest`.

Lines are formatted *before* the mutex is taken; only the sink write is
serialised, so concurrent logging never tears a line
(`LoggerConcurrencyTest`).

### 3.3 Secrets — hard rule

**Never logged:** full publish secret / stream key, full bearer token,
sensitive query string, any private credential.

The logger cannot know what is sensitive, so redaction is mandatory *at the
call site* using:

| Helper | Behaviour |
|---|---|
| `redact(s)` | `"abcd…(len=28)"` — ≤4-char prefix, for correlation only. Values shorter than 8 chars are redacted entirely. |
| `redact_fully(s)` | `"[redacted]"` |
| `redact_query(uri)` | keeps path and parameter **names**, replaces every parameter **value** with `[redacted]` |

`redact_query` redacts *all* values regardless of parameter name. Allowlisting
by name (`expires` looks harmless today) is precisely how leaks appear later.

This is verified by an executed test, not by review:
`LoggerRedactionTest.AKnownSecretNeverAppearsInActualLogOutput` logs a
realistic publish key, a JWT-shaped bearer token, a password and a signed
playback URL through the sanctioned helpers, captures the real emitted output,
and asserts none of those strings appear anywhere in it.

**Audit of existing call sites (Phase 7).** Every `RTMP_LOG` site in `src/`
was inspected. No stream key, token, password or credential was being logged.
`ManagementApi` logs `request.path`, which `HttpRequest` defines as
query-free (`http_server.hpp:23-24`) — the query string is parsed into a
separate `query` member that is never logged. No fixes were required; the
redaction API and its test exist to keep it that way.

---

## 4. Operating the metrics

Recommended alerts:

| Condition | Meaning |
|---|---|
| `rate(slow_viewer_evictions[5m]) > 0` | viewers are being dropped; egress bandwidth or CPU is short |
| `rate(dropped_video_frames[5m])` rising | per-viewer queues are saturating |
| `outbound_queue_bytes` trending up | fan-out cannot keep pace with ingest |
| `rate(authentication_failures[5m])` spike | credential stuffing / misconfigured encoder |
| `rate(recording_failures[5m]) > 0` | disk full, unwritable, or queue overflowing |
| `metrics_rejected_names_total > 0` | a code change is trying to emit a high-cardinality name — fix the call site |
| `rate(io_uring_sq_full[5m]) > 0` | submission queue too small for the offered load |
| `process_memory_bytes` trending up with flat `active_connections` | leak |

`gop_cache_bytes`, `outbound_queue_bytes` and `viewers_per_stream_*` are only
updated when something calls `LiveFanout::sample_gauges()`. Wire that to a
low-frequency timer (1–10 s); it takes per-stream locks and is O(streams +
subscribers), so it must not run on a network worker.
