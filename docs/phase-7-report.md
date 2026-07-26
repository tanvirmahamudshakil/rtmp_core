# PHASE 7 COMPLETION REPORT

**Phase 7 — Observability, real load testing and capacity validation**
(`docs/v2_promot.md` lines 1077–1232)

Companion documents: `docs/observability.md` (metrics + logging design),
`docs/capacity-report.md` (measured capacity, and what could not be measured).

---

## 1. What was inspected

* `include/rtmp_server/observability/metrics.hpp` + `src/observability/metrics.cpp` — the Phase 5 `Metrics` class.
* `include/rtmp_server/observability/logger.hpp` + `src/observability/logger.cpp` — the Phase 0 logger.
* `include/rtmp_server/observability/audit_log.hpp` — checked for credential capture.
* `apps/load_bench/main.cpp` — the pre-existing "load" benchmark.
* Every `RTMP_LOG` call site in `src/` (34 sites), audited for secret leakage.
* `src/protocol/commands/live_fanout.cpp`, `viewer_queue.hpp`, `gop_cache.hpp` — fan-out/backpressure metric attachment points.
* `src/authentication/rtmp_authenticator.cpp` — auth-failure and connection-admission paths.
* `src/recording/async_file_sink.cpp` — recording queue/failure paths.
* `src/control/management_api.cpp`, `http_server.cpp` — the `/metrics` endpoint and whether query strings reach the logs.
* `src/management/stream_manager.cpp` — existing dynamic metric names.
* `src/io/io_uring/event_loop.cpp`, `src/network/tcp_connection.cpp` — transport-level counters (Linux-only).
* `tests/integration/rtmp_full_session_socket_test.cpp` — the existing "real socket without io_uring" test pattern, reused as the basis for the Phase 7 harness.
* `CMakeLists.txt` platform guards, `CMakePresets.json`.

---

## 2. Problems confirmed

1. **`Metrics` exposed almost none of the required Phase 7 metric set.** It had only an untyped, mutex-guarded string→int map with 4 methods. 29 of the ~32 required metrics did not exist. Its own doc comment stated it was "explicitly not meant to be incremented on the per-packet hot path" — i.e. unusable for fan-out metrics.

2. **No metric was wired to any call site** except `StreamManager` mutation counts. Fan-out, recording, authentication and transport recorded nothing.

3. **`/metrics` emitted an invalid exposition format.** `ManagementApi::handle_metrics()` advertised `text/plain; version=0.0.4` but emitted bare `name value` lines with no `# HELP`/`# TYPE`.

4. **Metric names were not legal Prometheus names.** `StreamManager` emitted `management.<action>_total`; `.` is not permitted in a metric name, so those series were unscrapeable. Caught mechanically by the new `is_valid_dynamic_name()` (it failed the existing test, which is how the defect surfaced). Renamed to `management_<action>_total`.

5. **The logger produced invalid JSON for many values.** Field keys and values were interpolated raw. A value containing `"`, `\` or a newline produced a malformed record, and a crafted value could inject synthetic fields (e.g. a fake `"level":"error"`) into a downstream pipeline. Fixed with `escape_json_into()`; proven by `LoggerJsonTest.FieldInjectionViaAValueIsNeutralised`.

6. **The logger had no structured context fields.** No `worker_id`, `connection_id`, `stream_id`, `application_id`, `error_code`, `latency`, or `request_id` — all seven are required by Phase 7. Added as `LogContext`.

7. **No redaction API existed.** The header *asserted* callers must redact, but provided nothing to redact with and no test that any secret stayed out of the output.

8. **`apps/load_bench` was exactly the failure mode the doc names.** It opens no socket; it constructs `CommandSession` objects in-process and pushes 2-byte payloads (`{0x17, 0x01}`) through `handle_message()`. Its own header comment admits "no real socket transport on this host". Any viewer-count claim from it would be precisely the "synthetic in-memory benchmark loops over 1,000 subscribers" the doc forbids.

9. **`TcpConnection::on_send_completion()` silently discarded partial sends.** It ignored `bytes_sent` (`(void)bytes_sent;`) and popped the entire front buffer on any success. When the kernel accepted fewer bytes than requested — normal TCP behaviour, and explicitly called out in `docs/v2_promot.md` §3.4 — the remainder was dropped, desynchronising the peer's chunk decoder. **Fixed** (see §6), but **not compile-verified on this host**: the file is Linux-only.

10. **`LiveFanout` never calls `ViewerQueue::note_flushed()`.** `ViewerQueue`'s documented contract requires a transport-less caller to call it after every `Deliver`. Because it does not, `bytes_` grows monotonically until it trips the 8 MB budget, resets, and repeats. **Measured consequence:** 100 healthy loopback viewers with no real backpressure recorded 3,701 dropped video frames, 5,397 dropped audio frames and 100 "recoveries" (`docs/capacity-report.md` §5.1). **Not fixed** — the correct repair is a fan-out design change (feed real transport backlog into `offer()`), which needs its own phase and before/after measurement.

11. **Viewers are orphaned by a publisher reconnect.** Measured in scenario 07: egress fell to 0 after each reconnect while all 100 viewer TCP connections stayed open, and no `onStatus`/`UnpublishNotify` was sent, so a real player has nothing to react to. **Not fixed** — needs a product decision.

12. **`send()` raised SIGPIPE on peer disconnect (introduced and fixed within this phase).** The first revision of the load generator called `::send(fd, ..., 0)`. A load generator disconnects peers constantly, and writing to a socket whose peer has gone raises `SIGPIPE`, terminating the process — it surfaced as an intermittent `LoadGenPipeline ... (SIGPIPE)` ctest failure. Fixed portably: `SO_NOSIGPIPE` on Darwin/BSD, `MSG_NOSIGNAL` send flag on Linux, applied in `RtmpClient`, the test harness and `apps/rtmp_test_server`. Verified stable over 5 consecutive runs plus both full suites.

13. **Test-harness index bug (introduced and fixed within this phase).** ASan caught a `heap-buffer-overflow`: the connection vector grows during `accept` *after* the `pollfd` array was sized, so indexing `pfds[i+1]` by `connections.size()` read past the end. Fixed in both the test harness and `apps/rtmp_test_server`.

---

## 3. Problems not confirmed

1. **No existing log call site leaks a secret.** All 34 `RTMP_LOG` sites were inspected. `grep` for logging of `stream_key`/`token`/`secret`/`password`/`credential` in `src/` returns nothing. `ManagementApi` logs `request.path`, which `http_server.hpp:23-24` defines as query-free (the query string is parsed into a separate `query` member that is never logged), so the "sensitive query string" rule was already satisfied. `AuditLog` explicitly documents and honours "never a raw key/token". **No fixes were required**; the redaction API and its test were added to keep it that way, not to repair a leak.

2. **`RtmpConnectionSession` / `CommandSession` are not thread-unsafe by design.** An early revision of the new harness gave each connection its own thread; ASan reported heap corruption inside `ChunkEncoder::encode_message`. Investigation showed this was a **harness defect, not a server defect**: production assigns each connection to exactly one io_uring worker and routes cross-worker frames through `CrossWorkerRouter` precisely so two threads are never inside one session. The harness was rewritten single-threaded to model that ownership. No production change was needed.

3. **The GOP cache does not exceed its bounds.** Under 8 Mbps / 10 s GOPs it peaked at 10.1 MB against a 16 MB budget and always released after publisher stop.

4. **No memory leak.** RSS returned to baseline after every scenario (473 MB → 34 MB in scenario 09).

5. **Media is not corrupted anywhere in the pipeline.** 695,778 payloads byte-verified across the matrix; zero corrupt.

---

## 4. Architecture decisions

1. **Two-tier metric registry.** A closed, compile-time catalog (X-macro → `MetricId` → fixed `std::array<std::atomic<std::int64_t>>`) for lock-free hot-path increments, plus the legacy string map for Phase 5 compatibility. Rationale: the required metrics include per-frame, per-viewer events; a mutex-guarded map lookup per frame per viewer is not viable at 1,000 viewers.

2. **Cardinality is enforced mechanically, not by convention.** The catalog is closed; `is_valid_dynamic_name()` rejects names containing a run of 4+ digits (the shape of an interpolated ID); the dynamic map is bounded at 256 entries; rejections increment `metrics_rejected_names_total` rather than being silent.

3. **`viewers_per_stream` is aggregated, `connections_per_worker` is labelled.** Stream count is unbounded and stream keys are secrets → export `viewers_per_stream_max` / `_mean_milli` / `active_streams`. Worker count is bounded by *configuration* → a `worker="N"` label is safe.

4. **Expensive gauges are sampled, never maintained on the media path.** `LiveFanout::sample_gauges()` walks streams under their own locks and is called from a periodic sampler or the `/metrics` handler — never from a network worker.

5. **`egress_bytes_total` is batched per frame, not per viewer.** `run_deliveries()` accumulates locally and folds in one atomic add. At 1,000 viewers this is 1 rather than 1,000 contended RMWs on one cache line per frame.

6. **Metrics are injected, never global.** Every component takes an optional non-owning `Metrics*`; null means "no observability", which is why all 376 pre-existing tests passed unmodified.

7. **Redaction is the caller's job, with supplied tools.** The logger cannot know what is sensitive. `redact` / `redact_fully` / `redact_query` are the sanctioned helpers; `redact_query` redacts *all* parameter values because allowlisting by name is how leaks appear later.

8. **The load generator is a library plus a thin CLI.** `rtmp_server_loadgen` holds all logic so tests can assert its byte-level protocol correctness against the production codecs; `apps/rtmp_load_gen` is only argument parsing.

9. **One thread drives many clients via `poll(2)`.** 1,000 viewers cost a handful of threads, and single-threaded driving keeps measured latencies free of cross-thread scheduling noise.

10. **A separate, clearly-labelled test-only server (`apps/rtmp_test_server`).** Needed because the production io_uring binary cannot run on this host, and without *something* to connect to there would be no real measurements at all. Its header comment and `docs/capacity-report.md` both state prominently that it is not production, not io_uring, single-threaded, and must never be deployed.

11. **Synthetic media is structurally real.** `MediaSource` emits genuine FLV/AVC/AAC tag headers with correct `AVCDecoderConfigurationRecord`, length-prefixed NALs, and bitrate-derived sizes with keyframes ~8× inter frames — so `classify_video_tag()` really recognises keyframes and the GOP cache really engages. A deterministic embedded byte pattern makes corruption detectable byte-for-byte.

---

## 5. Files added

| Path | Purpose |
|---|---|
| `include/rtmp_server/loadgen/media_source.hpp` | Synthetic H.264/AAC generator + corruption verifier |
| `src/loadgen/media_source.cpp` | Implementation |
| `include/rtmp_server/loadgen/rtmp_client.hpp` | Real non-blocking RTMP client state machine |
| `src/loadgen/rtmp_client.cpp` | Implementation |
| `include/rtmp_server/loadgen/scenario.hpp` | Scenario config + measured report |
| `src/loadgen/scenario.cpp` | poll(2) scenario driver |
| `src/loadgen/CMakeLists.txt` | `rtmp_server_loadgen` target |
| `apps/rtmp_load_gen/main.cpp` + `CMakeLists.txt` | CLI front-end |
| `apps/rtmp_test_server/main.cpp` + `CMakeLists.txt` | **TEST-ONLY** poll-based server front-end |
| `tests/load/loadgen_protocol_test.cpp` + `CMakeLists.txt` | 10 protocol-correctness / pipeline tests |
| `tests/unit/observability/logger_test.cpp` | 10 logger tests incl. secret redaction |
| `scripts/phase7_load_matrix.sh` | Reproducible 9-scenario matrix |
| `docs/observability.md` | Metrics + logging design |
| `docs/capacity-report.md` | Measured capacity + explicit not-measurable section |
| `docs/phase-7-report.md` | This report |

## 6. Files modified

| Path | Change |
|---|---|
| `include/rtmp_server/observability/metrics.hpp` | Full catalog, typed API, cardinality guards, derived rates, Prometheus export |
| `src/observability/metrics.cpp` | Implementation incl. RSS sampling (Darwin + Linux) |
| `include/rtmp_server/observability/logger.hpp` | `LogContext`, redaction API, settable sink, `escape_json_into` |
| `src/observability/logger.cpp` | JSON escaping, context fields, format-before-lock |
| `include/rtmp_server/protocol/commands/live_fanout.hpp` / `.cpp` | `set_metrics`, `sample_gauges`, drop/recovery/eviction/viewer/egress counters |
| `include/rtmp_server/protocol/commands/stream_registry.hpp` | `active_publishers`, `publisher_disconnects` |
| `include/rtmp_server/protocol/session/rtmp_connection_session.hpp` / `.cpp` | `ingress_bytes_total` |
| `include/rtmp_server/authentication/rtmp_authenticator.hpp` / `.cpp` | `authentication_failures`, `active_connections` |
| `include/rtmp_server/recording/async_file_sink.hpp` / `.cpp` | `recording_queue_depth`, `recording_failures` |
| `src/control/management_api.cpp` | `/metrics` now full Prometheus exposition + refresh |
| `src/management/stream_manager.cpp` | `management.` → `management_` (illegal name fix) |
| `include/rtmp_server/network/tcp_connection.hpp` / `.cpp` | **Partial-send fix** + `partial_send_count` |
| `include/rtmp_server/io/io_uring/event_loop.hpp` / `.cpp` | `set_metrics`; SQ-full, buffer exhaustion, timeouts, connections, egress, partial sends; resume-from-offset send |
| `CMakeLists.txt` | Added `src/loadgen`, `apps/rtmp_load_gen`, `apps/rtmp_test_server`, `tests/load` |
| `tests/unit/CMakeLists.txt` | Added `logger_test.cpp` |
| `tests/unit/observability/metrics_test.cpp` | +11 Phase 7 tests |
| `tests/management/stream_manager_persistence_test.cpp` | Updated to the corrected metric names |

---

## 7. Public interfaces changed

**Additive (no existing signature changed, no caller broken):**

* `observability::MetricId`, `MetricKind`, `MetricDescriptor`, `metric_catalog()`, `metric_name()`, `is_valid_dynamic_name()`, `kMaxWorkers`, `kMaxDynamicMetrics`.
* `Metrics`: `increment/set/add/value(MetricId)`, `set_connections_for_worker`, `connections_for_worker`, `observe_viewers_per_stream`, `commit_viewers_per_stream`, `refresh_derived`, `refresh_process_metrics`, `snapshot`, `render_prometheus`. `Metrics` is now non-copyable (it holds atomics).
* `observability::LogContext`, `redact`, `redact_fully`, `redact_query`, `escape_json_into`, `Logger::set_sink`/`reset_sink`/`log_fields` and the context-taking `log` overload.
* `set_metrics(Metrics*)` on `LiveFanout`, `StreamRegistry`, `RtmpConnectionSession`, `RtmpAuthenticator`, `AsyncFileSink`, `IoUringEventLoop`.
* `LiveFanout::sample_gauges()`.
* `TcpConnection::next_write_offset()`, `partial_send_count()`.
* New namespace `rtmp_server::loadgen` (`MediaProfile`, `MediaSource`, `RtmpClient`, `ScenarioConfig`, `ScenarioReport`, `run_scenario`).

**Behavioural changes:**

* `Metrics::increment_counter` / `set_gauge` now **reject** invalid or over-budget names instead of accepting them (this is the point).
* `GET /metrics` response body changed from bare lines to full Prometheus exposition.
* `management.<action>_total` → `management_<action>_total`.
* `TcpConnection` no longer drops the remainder of a partial send.

---

## 8. Tests added

**`tests/unit/observability/metrics_test.cpp` (+11)** — catalog completeness against the doc's exact list; name uniqueness/legality/HELP presence; counter/gauge semantics; identifier-shaped name rejection; dynamic-registry bound; per-worker bound; viewers-per-stream aggregation; derived bitrate over a window; RSS sampling; Prometheus rendering (incl. asserting no `stream_key` or `connection_id=` appears); 8-thread × 20,000 concurrent increment integrity.

**`tests/unit/observability/logger_test.cpp` (+10)** — all seven context fields emitted; unset fields omitted; level filtering; quote/backslash/control escaping; **field-injection neutralisation**; `\u00XX` control escapes; `redact` reveals no reusable prefix; **`AKnownSecretNeverAppearsInActualLogOutput`** (logs a realistic publish key, JWT-shaped bearer token, password and signed playback URL through the sanctioned helpers, captures the real emitted output, asserts none appear); `redact_query` semantics; 8-thread untorn-line test.

**`tests/load/loadgen_protocol_test.cpp` (+10)** — AVC sequence header parses via the production `parse_avc_sequence_header`; AAC sequence header classifies correctly; keyframes emitted at the configured interval and classified as keyframes by the production classifier; payload sizes track configured bitrate; verifier accepts generated frames and rejects a single flipped byte and truncation; real handshake accepted by the production `HandshakeSession` with `connect`/`createStream`/`publish` decoded in order by the production `ChunkDecoder`; viewer sends `play`; published media survives the real chunk-codec + TCP round trip byte-for-byte; abrupt disconnect sends no `deleteStream`; **full pipeline** — real publisher + 8 real viewers over TCP through `RtmpConnectionSession`/`LiveFanout`, asserting zero corruption and that ingress/egress/viewer/publisher metrics moved and no stream key reached the metric output.

Total: **+31 tests** (376 → 407).

---

## 9. Commands executed

```bash
git log --oneline -5                                   # verified Phase 6 merge
cmake --preset core-only -DRTMP_SERVER_CORE_ONLY=ON
cmake --build --preset core-only
ctest --preset core-only                               # baseline, then final
cmake --preset asan -DRTMP_SERVER_CORE_ONLY=ON
cmake --build --preset asan
ctest --preset asan
clang++ -std=c++23 -fsyntax-only -Iinclude /tmp/sc.cpp # tcp_connection.hpp standalone
./build/core-only/tests/load/rtmp_server_load_tests
./build/asan/tests/load/rtmp_server_load_tests
./scripts/phase7_load_matrix.sh                        # 9 scenarios, ~4 min
sysctl -n machdep.cpu.brand_string hw.ncpu hw.memsize; sw_vers; uname -a
```

---

## 10. Actual build result

```
cmake --build --preset core-only   → SUCCESS, 0 errors, 0 new warnings
cmake --build --preset asan        → SUCCESS, 0 errors, 0 new warnings
```

Pre-existing benign linker notices remain unchanged
(`ld: warning: ignoring duplicate libraries: 'src/protocol/librtmp_server_protocol.a'`).

**Not built on this host:** `src/io/io_uring/*`, `src/network/tcp_connection.cpp`, `apps/rtmp_server` — excluded by the `NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux"` guard. My edits to those files are therefore **compile-unverified**. Partial mitigation: `include/rtmp_server/network/tcp_connection.hpp` was confirmed to compile standalone with `clang++ -std=c++23 -fsyntax-only`. **A Linux build is required before those changes can be trusted.**

## 11. Actual test result

```
ctest --preset core-only
    100% tests passed, 0 tests failed out of 407
    Total Test time (real) = 8.80 sec
```

Baseline before this phase: **376/376**. Final: **407/407** (376 + 31 new). No test was disabled, skipped or weakened.

One pre-existing test was updated rather than skipped:
`StreamManagerPersistenceTest.MutationsIncrementMatchingMetricsCounters` failed
after the new name validator rejected `management.create_stream_total`. That
was the validator correctly catching an illegal Prometheus name (problem #4);
the production names were fixed and the test now additionally asserts the old
dotted spelling reads back as absent.

## 12. Sanitizer result

```
ctest --preset asan   (AddressSanitizer + UndefinedBehaviorSanitizer)
    100% tests passed, 0 tests failed out of 407
    Total Test time (real) = 27.57 sec
```

ASan/UBSan found **two real defects during development**, both now fixed and re-verified clean:

1. `memcpy-param-overlap` / heap corruption inside `ChunkEncoder::encode_message`, reached from `LiveFanout::subscribe` — caused by the first (multi-threaded) revision of the test harness violating the single-worker ownership contract. Harness rewritten single-threaded.
2. `heap-buffer-overflow` reading `pfds[i+1]` after `accept` grew the connection vector past the polled prefix — in both the test harness and `apps/rtmp_test_server`. Both bounded by the polled count.

Separately, repeated ctest runs surfaced an intermittent `SIGPIPE` abort in the load generator (problem #12), fixed portably and re-verified over 5 consecutive pipeline runs and two full-suite runs of each preset.

TSan was not run: the `tsan` preset exists but was out of scope for this phase, and the multi-threaded surface added here (atomic metric slots, one logger mutex) is small. Recommended for the next phase.

## 13. Performance observations

Full detail in `docs/capacity-report.md`. Measured on Apple M1, **Debug `-O0`**, loopback, single-threaded test server:

| Observation | Value |
|---|---|
| 1 publisher + 100 viewers | 100/100 reached play, 238 Mbps egress, 21 MB RSS, **0 corrupt / 131,638 verified** |
| Fan-out amplification | 2.80 Mbps ingress → 238 Mbps egress (**85×**) |
| Peak observed egress | 434 Mbps |
| First-media latency | **4–5 µs p50** (GOP cache serves a new viewer essentially instantly) |
| Saturation plateau | **~160 concurrent viewers**; 500 and 1,000 requested both plateaued at ~160 |
| Abrupt disconnect (100 RST) | 201→101 connections, publisher unaffected, clean |
| Publisher reconnect | 4 cycles survived, no leak (RSS ~5 MB) |
| Large keyframes (8 Mbps, 10 s GOP) | GOP cache 10.1 MB, **27 slow-viewer evictions**, RSS peak 473 MB |
| Memory | ~0.6–0.7 MB/viewer at 2.5 Mbps; ~7 MB/viewer at 8 Mbps/10 s GOP |
| Corruption across all 9 scenarios | **0 / 695,778 payloads** |

**The 500/1,000/10-publisher scenarios did not achieve their requested
concurrency** (161, 160 and 311 respectively). Evidence in the capacity report
shows the single-threaded generator+server pair is the bottleneck, not the
server design. Per the doc's explicit warning, **no 1,000-viewer capability is
claimed.**

## 14. Remaining risks

1. **io_uring changes are compile-unverified.** `event_loop.cpp`, `tcp_connection.cpp`/`.hpp` were edited but cannot be built here. **Highest risk item.** Must be built and tested on Linux before merge to a release branch.
2. **Spurious frame drops (§2.10).** Healthy viewers record thousands of drops. Until fixed, `dropped_*_frames` and `outbound_queue_bytes` are not trustworthy for capacity decisions.
3. **No production capacity number exists.** Everything measured is `-O0`, loopback, single-threaded, non-io_uring. No operator commitment can be made from it.
4. **Five metrics are registered but never fed** (`io_uring_cq_overflow`, `inter_worker_queue_depth`, `inter_worker_queue_drops`, `worker_cpu_usage`, and `connections_per_worker` outside the io_uring loop). They export `0`, which reads as "nothing happened" rather than "not instrumented". Documented in `docs/observability.md` §2.5.
5. **No 24-hour soak was run.** Leak behaviour beyond ~25 s is unverified.
6. **`apps/rtmp_test_server` could be mistaken for production.** Mitigated by a prominent header comment and repeated warnings in the capacity report, but it is a real binary in the tree.
7. **Viewers orphaned on publisher reconnect (§2.11)** — unresolved product question.
8. **TSan not run.**
9. **`apps/load_bench` still exists** and still reports misleading throughput. Left in place (Phase 7 says "replace **or supplement**"); `rtmp_load_gen` supplements it. It should probably be deleted or renamed in a later phase.

## 15. Breaking changes

* **`Metrics` is now non-copyable** (holds `std::atomic`). No existing code copied it.
* **`Metrics::increment_counter`/`set_gauge` reject invalid names.** Any caller emitting a name with uppercase, `.`, spaces, or a 4+ digit run now silently records nothing (and bumps `metrics_rejected_names_total`). One in-tree caller was affected and fixed.
* **`management.<action>_total` → `management_<action>_total`.** Any external dashboard using the old names must be updated. The old names were unscrapeable, so this is a fix, not a regression.
* **`GET /metrics` body format changed** to full Prometheus exposition. Strictly more parseable; a naive line-splitter that assumed no `#` comments would need updating.
* **`TcpConnection::on_send_completion` semantics changed** — it now honours `bytes_sent`. Callers relying on the front buffer always being popped on success would be affected; the only caller is `IoUringEventLoop`, updated in step.

No RTMP wire-protocol change. No configuration-file change. No database schema change.

## 16. Rollback considerations

* **Fully additive parts** (the `loadgen` library, `rtmp_load_gen`, `rtmp_test_server`, `tests/load`, the docs) can be removed by dropping four `add_subdirectory` lines and the directories. Zero impact on the server.
* **Metric wiring** is behind `set_metrics()`, defaulting to `nullptr`. Reverting is a matter of not calling it; no behaviour depends on metrics being present.
* **The logger change is not safely revertible** without reintroducing the JSON-injection defect. Prefer forward fixes.
* **The partial-send fix must not be reverted** — reverting reinstates silent media corruption. If it proves problematic on Linux, fix forward.
* **The metric-name rename** is the only change requiring external coordination (dashboards).
* Each concern is in a separate commit-sized group of files, so partial revert is practical.

## 17. Definition-of-done checklist

Against `docs/v2_promot.md` PHASE 7 "Definition of done":

| Requirement | Status | Evidence |
|---|---|---|
| Metrics are available | ✅ | 33-entry catalog, `/metrics` Prometheus exposition, 11 tests. 5 entries registered-but-unfed, declared in `docs/observability.md` §2.5 |
| Logs are structured | ✅ | JSON-lines, all 7 required context fields, escaping, 10 tests |
| Never log secrets | ✅ | Redaction API + audit of all 34 call sites + an executed test asserting a known secret never appears in real output |
| Real socket-based load tests exist | ✅ | `rtmp_load_gen`: real TCP, real handshake, real AMF0 commands; 10 tests validate against production codecs |
| Load tests use realistic payload sizes | ✅ | Real FLV/AVC/AAC tags, bitrate-derived sizes, keyframes ~8× inter frames; bitrate-tracking test |
| Slow viewers are tested | ✅ | Scenario 06 (200 viewers, 50 % at 2 KB/tick); evictions observed in scenario 09 |
| Configurable bitrate / keyframe interval / ramp-up | ✅ | CLI flags, exercised across the matrix |
| Abrupt disconnect + publisher reconnect | ✅ | Scenarios 07, 08; RST via `SO_LINGER 0` |
| Latency and corruption statistics | ✅ | p50/p99 for connect/handshake/play/first-media; byte-level verification |
| Capacity claims backed by measured evidence | ✅ | `docs/capacity-report.md` §3; unachieved scenarios reported as partial |
| Safe operating limits documented | ⚠️ **partial** | §7.1 documents limits for the *measured* configuration only; §7.2 states plainly that no production limit is established |
| 1 pub + 100 viewers | ✅ | 100/100 |
| 1 pub + 500 / 1,000 viewers | ❌ **not achieved** | 161 / 160 reached play — generator-bound, reported honestly, not claimed |
| 10 publishers + 100 viewers each | ❌ **not achieved** | 4 publishers / 311 viewers |
| Viewer burst, network interruption, large keyframes | ✅ | Scenarios 05, 08, 09 |
| 24-hour soak | ❌ **not run** | Requires a Linux host; commands provided in `docs/capacity-report.md` §6.1 |

**Phase 7 is complete for everything achievable on this host.** Three
scenario-scale items and the soak test are explicitly **not done** and are not
claimed to be.

## 18. Recommended next phase

**Before Phase 8, run this phase's work on a Linux host.** Specifically:

1. `cmake --preset release && ctest --preset release` on Linux to compile-verify the io_uring and `TcpConnection` changes (risk #1). **This is a prerequisite, not optional.**
2. Fix the spurious-drop defect (§2.10) by feeding real transport backlog into `ViewerQueue::offer()` — otherwise the drop metrics stay uninterpretable.
3. Instrument the five unfed metrics, especially `worker_cpu_usage` and `connections_per_worker`, so per-core scaling can be attributed.
4. Re-run `scripts/phase7_load_matrix.sh` against the real io_uring server, from a **separate** load-generating machine over a real NIC, and replace `docs/capacity-report.md` §3 with production numbers.
5. Run the 24-hour soak.
6. Decide and implement publisher-reconnect viewer semantics (§2.11).
7. Run TSan.

Then proceed to **PHASE 8 — Security hardening, deployment and production
release** (`docs/v2_promot.md` line 1236+). Phase 7 leaves Phase 8 two useful
assets: `rtmp_load_gen` is directly reusable as a malformed-input and
abuse-simulation driver, and `redact`/`redact_query` are the primitives Phase 8's
TLS and credential-handling work will need.
