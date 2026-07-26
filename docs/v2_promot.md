# MASTER PROMPT — Production-Grade C++23 RTMP Streaming Server

You are acting as a **Principal C++ Systems Engineer, Streaming Media Architect, Linux Networking Engineer, and Production Reliability Engineer**.

You have been given an existing C++ RTMP server repository named:

`rtmp_core-main`

Your job is to transform this repository into a **professional, production-grade, scalable RTMP streaming server** capable of supporting a large number of concurrent viewers, subject to the physical CPU, memory, and network bandwidth available on the server.

The project must be improved incrementally. Do not rewrite the entire repository blindly.

---

# 1. Primary objective

Build a reliable streaming platform with the following flow:

```text
OBS / RTMP Encoder
        │
        ▼
RTMP Ingest Server
        │
        ├── Publish authentication
        ├── RTMP handshake
        ├── Chunk decoding
        ├── AMF command processing
        ├── Media ingest
        ├── Stream registry
        ├── GOP cache
        └── Shared media frames
                │
                ├── RTMP egress worker shards
                │       └── RTMP viewers
                │
                ├── FLV recording
                │
                └── Optional HLS/LL-HLS packaging
                        └── HTTP/CDN viewers
```

The final server must prioritise:

* Correctness
* Stable connection lifecycle
* Bounded memory usage
* Efficient fan-out
* Proper backpressure
* Multi-core scalability
* Secure publish and playback authentication
* Production observability
* Realistic load testing
* Graceful failure handling
* Maintainable architecture

---

# 2. Important existing issues to verify

Do not blindly assume these findings are correct. Inspect the repository and verify each one.

The existing project may currently have the following problems:

1. The executable RTMP server may only complete the RTMP handshake.
2. `ChunkDecoder`, `ChunkEncoder`, `CommandSession`, media ingest and `LiveFanout` may not be connected to the actual socket lifecycle.
3. RTMP bytes arriving together with C2 may be discarded after handshake completion.
4. Publish URLs may use a secret stream key while playback URLs use a public stream name, without mapping both to the same internal stream ID.
5. `LiveFanout` may invoke subscriber callbacks while holding a global mutex.
6. Media payloads may be deeply copied once per viewer.
7. TCP partial sends may not be handled correctly.
8. Per-viewer outbound queues may be unbounded.
9. Only one `io_uring` event-loop thread may actually run, even when worker count exists in configuration.
10. Some configuration options may be parsed but not enforced.
11. Playback tokens may be generated but not validated in the RTMP `play` flow.
12. The management library may not be linked to the actual server executable.
13. The load benchmark may use synthetic two-byte payloads and therefore may not represent real network capacity.
14. Connection shutdown may not correctly remove publishers and subscribers.
15. Raw pointers may be used for subscriber/session ownership.
16. There may be no real HTTP management API, metrics endpoint or readiness endpoint.

Verify these findings from the actual code before modifying anything.

---

# 3. Non-negotiable engineering rules

Follow these rules throughout every phase.

## 3.1 Do not fake completion

Never claim that something works unless you have:

* Implemented it
* Compiled it
* Run the relevant tests
* Reported the actual test output
* Documented any untested area

Never write placeholder production code such as:

```cpp
// TODO: implement later
return true;
```

Do not report a test as passing unless it was actually executed.

---

## 3.2 Preserve working functionality

Do not remove working functionality unless replacement is necessary and clearly justified.

Before changing a component:

1. Explain its current role.
2. Explain the defect or limitation.
3. Explain the proposed change.
4. Identify compatibility risks.
5. Add or update tests.
6. Implement the change.
7. Run validation.

---

## 3.3 Use production-quality C++23

Use:

* RAII
* Clear ownership
* Move semantics
* `std::span`
* `std::expected` where appropriate
* `std::shared_ptr<const T>` only where shared lifetime is actually required
* `std::unique_ptr` for exclusive ownership
* Strong types for stream IDs, connection IDs and subscriber IDs
* Safe integer conversion
* Explicit error handling
* `std::chrono` for time values
* `std::atomic` only when the memory model is justified

Avoid:

* Owning raw pointers
* Detached threads
* Global mutable state
* Callbacks under locks
* Blocking disk or database I/O on network event-loop threads
* Unlimited queues
* Unbounded allocations based on client-controlled values
* Busy loops
* Silent error swallowing
* Exceptions crossing C callback or kernel completion boundaries
* Unnecessary singleton patterns
* Undefined connection ownership
* Deep-copying large media frames per viewer

---

## 3.4 Network correctness

The server must correctly handle:

* Partial reads
* Partial writes
* `EAGAIN`
* `EINTR`
* `ECONNRESET`
* `EPIPE`
* `ECANCELED`
* Timeouts
* Peer disconnects
* Duplicate completion events
* Late completions after logical shutdown
* Half-closed connections
* Write queue cancellation
* Graceful server shutdown

A successful send completion does not automatically mean the entire requested buffer was transmitted.

---

## 3.5 Bounded resource usage

Every resource controlled by a remote client must have a limit:

* RTMP message size
* Chunk size
* AMF nesting depth
* AMF string length
* Per-connection receive buffer
* Per-viewer outbound queue bytes
* Per-viewer outbound queue packets
* Per-stream GOP bytes
* Per-stream GOP duration
* Per-stream GOP packets
* Viewers per stream
* Publishers per application
* Connections per IP
* Authentication attempts
* Idle connection duration
* Publisher inactivity duration
* Pending management requests
* Recording queue size

The server must reject or disconnect abusive clients rather than consume unlimited memory.

---

## 3.6 Do not block event-loop threads

The following must not block network workers:

* SQLite/PostgreSQL operations
* FLV disk writes
* HLS segment writes
* DNS resolution
* HTTP API processing that performs database access
* Expensive token computation
* Long-running transcoding operations
* Logging flushes

Use bounded worker queues and dedicated threads where needed.

---

## 3.7 No lock-held subscriber callbacks

Never execute connection, subscriber or application callbacks while holding a fan-out registry mutex.

Correct pattern:

```cpp
std::vector<SubscriberHandle> targets;

{
    std::lock_guard lock(stream_mutex);
    targets = copy_subscriber_handles();
}

for (auto& target : targets) {
    target.enqueue(frame);
}
```

Prefer stream ownership or worker sharding over a global mutex on the hot media path.

---

## 3.8 No deep media copies per viewer

Represent media payloads with immutable shared storage.

Target concept:

```cpp
struct SharedMediaFrame {
    StreamId stream_id;
    MediaType media_type;
    std::uint32_t timestamp;
    bool is_keyframe;
    bool is_sequence_header;
    std::shared_ptr<const SharedBuffer> payload;
};
```

Each viewer queue should contain lightweight references, not a full payload copy.

Viewer-specific RTMP headers may be separate from the shared payload.

Use vectored writes where practical:

```text
[viewer-specific RTMP header] + [shared immutable media payload]
```

---

# 4. Required working method

Complete exactly one phase at a time.

For every phase:

1. Inspect the relevant code.
2. Produce a short implementation plan.
3. Identify files that will change.
4. Add or update tests before or alongside implementation.
5. Implement the phase.
6. Build the project.
7. Run tests.
8. Run sanitizers where applicable.
9. Update documentation.
10. Produce a phase report.
11. Stop before starting the next phase.

Do not combine multiple phases into one uncontrolled refactor.

After finishing each phase, wait for the instruction:

```text
CONTINUE TO PHASE N
```

---

# 5. Required phase report format

At the end of every phase, output:

```text
PHASE N COMPLETION REPORT

1. What was inspected
2. Problems confirmed
3. Problems not confirmed
4. Architecture decisions
5. Files added
6. Files modified
7. Public interfaces changed
8. Tests added
9. Commands executed
10. Actual build result
11. Actual test result
12. Sanitizer result
13. Performance observations
14. Remaining risks
15. Breaking changes
16. Rollback considerations
17. Definition-of-done checklist
18. Recommended next phase
```

Also create:

```text
docs/phase-N-report.md
```

Do not hide failed tests. Include relevant error output.

---

# PHASE 0 — Repository audit and reproducible baseline

## Objective

Understand the complete repository before making architectural changes.

## Tasks

1. Extract and inspect the complete repository.
2. Identify:

   * Executables
   * Static/shared libraries
   * CMake targets
   * RTMP protocol components
   * Networking components
   * Persistence components
   * Management components
   * Media components
   * Tests
   * Benchmarks
   * Deployment files
3. Trace the real runtime path from:

   * `main()`
   * listen socket
   * accept
   * connection creation
   * handshake
   * RTMP parsing
   * publish
   * play
   * media fan-out
   * socket write
   * disconnect
4. Draw the existing architecture in:

   * `docs/current-architecture.md`
5. Create the target architecture in:

   * `docs/target-architecture.md`
6. Create a gap analysis:

   * `docs/production-gap-analysis.md`
7. Build the repository without hiding errors.
8. Fix only minimal build-system issues required to establish a baseline.
9. Run all existing tests.
10. Run the existing benchmark and explain what it actually measures.
11. Record:

* Compiler
* CMake version
* Kernel version
* liburing version
* OpenSSL version
* SQLite version
* Build type

12. Generate `compile_commands.json`.
13. Check for:

* Compiler warnings
* Linker errors
* Missing source files
* Unused libraries
* Unlinked management modules
* Configuration fields that are never consumed

14. Produce a table mapping every configuration option to the code that enforces it.

## Required build configurations

Attempt:

```bash
cmake -S . -B build-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure
```

Also create an AddressSanitizer and UndefinedBehaviorSanitizer build if supported.

## Definition of done

* Repository builds reproducibly or exact blockers are documented.
* All existing tests have been executed.
* Runtime path is documented.
* CMake target graph is documented.
* Existing architecture and target architecture are documented.
* Known findings are confirmed or rejected with file and line references.
* No major runtime architecture refactor has started yet.

Stop after Phase 0.

---

# PHASE 1 — Complete RTMP connection and session pipeline

## Objective

Make the executable server perform a complete RTMP connection lifecycle after handshake.

## Target pipeline

```text
TcpConnection
    ↓
HandshakeSession
    ↓
RtmpConnectionSession
    ├── ChunkDecoder
    ├── ChunkEncoder
    ├── CommandSession
    ├── Control message handler
    ├── Media ingest handler
    └── Connection lifecycle handler
```

## Tasks

1. Create a clear `RtmpConnectionSession` abstraction.
2. Define explicit ownership between:

   * Event loop
   * TCP connection
   * Handshake session
   * RTMP session
   * Command session
   * Publisher
   * Subscriber
3. Preserve bytes received after C2 in the same TCP read.
4. Pass unconsumed handshake bytes into `ChunkDecoder`.
5. Connect `ChunkDecoder` to real socket input.
6. Connect decoded RTMP messages to:

   * Control handlers
   * AMF command handlers
   * Audio/video/data handlers
7. Connect `ChunkEncoder` to real socket output.
8. Wire:

   * `connect`
   * `createStream`
   * `publish`
   * `play`
   * `deleteStream`
   * `closeStream`
   * `FCUnpublish`, where supported
9. Implement required RTMP control messages:

   * Set Chunk Size
   * Window Acknowledgement Size
   * Set Peer Bandwidth
   * Acknowledgement
   * User Control Stream Begin
   * Ping Request
   * Ping Response
10. Enforce maximum RTMP message size.
11. Reject malformed chunk streams safely.
12. Ensure session teardown removes all publisher and viewer registrations.
13. Ensure a late kernel completion cannot access destroyed session memory.
14. Ensure one physical connection cannot accidentally create conflicting publisher states.
15. Add integration tests that feed real RTMP byte sequences.

## Required tests

At minimum:

* Handshake only
* Handshake plus `connect` in the same receive buffer
* Fragmented handshake
* Fragmented RTMP chunks
* Multiple chunks in one receive buffer
* `connect` command
* `createStream`
* Valid publish
* Valid play
* Malformed AMF
* Message exceeding configured maximum
* Abrupt disconnect during handshake
* Abrupt disconnect during publish
* Abrupt disconnect during play
* Ping/pong
* Connection cleanup

## Definition of done

* OBS can connect and publish through the real server executable.
* A real RTMP client can connect and issue `play`.
* Handshake trailing bytes are not lost.
* Socket input reaches the chunk decoder.
* Encoded RTMP output reaches the socket.
* Connection cleanup is deterministic.
* No use-after-free appears under ASan.
* All new integration tests pass.

Stop after Phase 1.

---

# PHASE 2 — Correct asynchronous TCP transport

## Objective

Make the `io_uring` TCP transport correct under real network behaviour.

## Tasks

1. Redesign write queue entries to track:

   * Buffer
   * Current offset
   * Remaining bytes
   * Operation ID
   * Cancellation state
2. Correctly handle partial sends.
3. Do not remove a write entry until every byte has been sent.
4. Guarantee only one ordered send operation per connection unless a documented safe batching model is used.
5. Correctly process:

   * `EAGAIN`
   * `EINTR`
   * `EPIPE`
   * `ECONNRESET`
   * `ECANCELED`
   * Zero-byte completion
6. Add connection-level output byte accounting.
7. Add configurable:

   * Write timeout
   * Read timeout
   * Idle timeout
   * Handshake timeout
8. Ensure timeout events are generation-safe and cannot close a newly reused connection object.
9. Implement graceful close:

   * Stop accepting new writes
   * Optionally flush a bounded amount
   * Cancel outstanding operations
   * Release resources after completions are drained
10. Ensure file descriptors are never double-closed.
11. Ensure stale completion entries cannot operate on a reused file descriptor.
12. Introduce stable connection IDs and operation generation IDs.
13. Add deterministic tests using:

* Socket pairs
* Very small socket send buffers
* Artificially slow readers
* Forced partial sends

14. Review receive-buffer ownership.
15. Prevent unbounded receive-buffer growth.

## Required tests

* 1-byte partial send progression
* Multiple partial sends
* Peer disconnect during send
* Queue cancellation
* Timeout during write
* Timeout during read
* Descriptor reuse safety
* Late CQE after logical shutdown
* Multiple queued messages remain ordered
* Large keyframe transmission
* Server shutdown with active connections

## Definition of done

* No payload corruption under partial sends.
* No lost bytes.
* No reordered writes.
* No double close.
* No stale completion use-after-free.
* All queues remain bounded.
* ASan, UBSan and relevant race checks are clean.

Stop after Phase 2.

---

# PHASE 3 — Stream identity, fan-out, GOP cache and backpressure

## Objective

Create a scalable and bounded media fan-out architecture.

## Stream identity requirements

Do not use a secret publish key as the permanent fan-out registry key.

Introduce strong internal IDs:

```cpp
struct ApplicationId;
struct StreamId;
struct PublisherId;
struct SubscriberId;
```

Required mapping:

```text
Publish secret key
        ↓ authentication
Internal StreamId
        ↑
Public playback name
```

Both publisher and viewer must resolve to the same internal `StreamId`.

## Fan-out tasks

1. Replace global hot-path locking with:

   * Per-stream ownership
   * Per-stream locks
   * Or worker-owned stream shards
2. Never call subscriber code while holding registry locks.
3. Replace raw subscriber pointers with safe handles.
4. Make unsubscribe idempotent.
5. Prevent callback invocation after subscriber destruction.
6. Use immutable shared media payloads.
7. Do not deep-copy payload data per viewer.
8. Maintain:

   * Metadata
   * Audio sequence header
   * Video sequence header
   * Latest GOP
9. Apply GOP limits:

   * Maximum bytes
   * Maximum duration
   * Maximum packets
10. Correctly identify:

* AVC keyframes
* AVC sequence headers
* AAC sequence headers
* Metadata

11. Define behaviour when no keyframe has arrived.
12. Define behaviour when codec headers change.
13. Ensure new viewers receive:

* Metadata
* Audio sequence header
* Video sequence header
* Cached GOP beginning at a keyframe

14. Implement bounded per-viewer queues.
15. Track queue bytes and packet count.
16. Implement slow-viewer policy.

## Required slow-viewer policy

A recommended policy:

```text
Normal state
    ↓ queue limit exceeded
Drop non-key video frames
    ↓
Enter waiting-for-keyframe state
    ↓
Discard video until next keyframe
    ↓
Send latest sequence headers
    ↓
Resume from keyframe
    ↓ still unable to recover
Disconnect slow viewer
```

Audio policy must be explicitly documented. Do not allow indefinite audio backlog.

## Required tests

* One publisher, multiple viewers
* Viewer joins before keyframe
* Viewer joins after keyframe
* Codec sequence header changes
* Metadata delivery
* Unsubscribe during dispatch
* Subscriber disconnect during dispatch
* Slow viewer recovery
* Slow viewer eviction
* GOP byte limit
* GOP packet limit
* GOP duration limit
* Publisher replacement policy
* Stream end cleanup
* No callbacks under lock
* Shared payload reference test
* Memory does not grow indefinitely

## Definition of done

* Publish key and playback name resolve to the same internal stream.
* Large media payloads are not deeply copied per viewer.
* Fan-out does not run subscriber callbacks under a global mutex.
* Per-viewer queues are bounded.
* Slow viewers cannot exhaust server memory.
* New viewers start from a valid GOP.
* Fan-out behaviour is covered by concurrency tests.

Stop after Phase 3.

---

# PHASE 4 — Multi-core `io_uring` worker architecture

## Objective

Scale network processing across CPU cores and avoid one event-loop bottleneck.

## Target architecture

```text
Main process
    ├── Acceptor or SO_REUSEPORT listener
    ├── Worker 0
    │     ├── io_uring
    │     ├── connections
    │     ├── timers
    │     └── egress stream shard
    ├── Worker 1
    ├── Worker 2
    └── Worker N
```

For highly popular streams:

```text
Publisher worker
      │
      ├── one shared frame reference → Egress worker 0
      ├── one shared frame reference → Egress worker 1
      ├── one shared frame reference → Egress worker 2
      └── one shared frame reference → Egress worker N
```

Do not send one cross-thread message per viewer.

## Tasks

1. Make `worker_ring_count` actually control worker creation.
2. Default worker count based on hardware concurrency, with a configurable upper bound.
3. Give every worker its own:

   * `io_uring`
   * connection map
   * timers
   * receive buffers
   * write queues
4. Choose and document:

   * Central accept plus dispatch
   * Or `SO_REUSEPORT`
5. Maintain connection affinity to one worker.
6. Implement bounded inter-worker queues.
7. Route each shared media frame once per target egress worker.
8. Avoid cross-worker access to mutable connection state.
9. Investigate and safely implement, where kernel support exists:

   * Multishot accept
   * Multishot receive
   * Provided buffer rings
   * Registered buffers
   * Completion batching
   * Submission batching
10. Detect unsupported kernel features and fall back safely.
11. Do not enable send zero-copy until lifetime and completion notification semantics are fully correct.
12. Pinning workers to CPU cores may be configurable but must not be mandatory.
13. Implement graceful worker shutdown.
14. Ensure configuration fields are genuinely enforced.
15. Add worker-level metrics.

## Required tests

* Multiple workers accept connections
* Connection stays on assigned worker
* Publisher and viewers on different workers
* Cross-worker fan-out
* Inter-worker queue saturation
* Worker shutdown
* Unsupported multishot fallback
* Provided-buffer exhaustion
* High connection churn
* No shared mutable connection access
* No deadlocks during stream shutdown

## Definition of done

* More than one `io_uring` worker actually runs.
* Viewer connections are distributed across workers.
* Popular stream egress can use multiple workers.
* Cross-worker queues are bounded.
* No connection object is concurrently mutated by multiple workers.
* Fallbacks work on kernels without optional features.

Stop after Phase 4.

---

# PHASE 5 — Persistence, authentication and management API

## Objective

Create a secure control plane separated from the media data plane.

## Required domain model

At minimum:

```text
Application
Stream
Publish credential
Playback policy
Active publisher session
Active viewer session
Recording policy
Stream status
Audit event
```

## Authentication tasks

1. Store publish keys securely:

   * Prefer hashed keys
   * Never log full secret keys
2. Support secure key rotation.
3. Resolve publish credentials to an internal `StreamId`.
4. Resolve public playback names to the same `StreamId`.
5. Parse playback query parameters safely.
6. Validate signed playback tokens.
7. Validate token expiry.
8. Use constant-time signature comparison.
9. Include suitable claims:

   * Application
   * Stream ID
   * Expiry
   * Optional session ID
   * Optional IP restriction
10. Enforce:

* Stream enabled state
* Application enabled state
* Maximum publishers
* Maximum viewers
* Maximum connections per IP

11. Add authentication failure rate limiting.
12. Never perform blocking database calls on media workers.
13. Use an in-memory immutable snapshot/cache for hot-path authorisation.
14. Refresh configuration safely from the control plane.

## Management API

Create a production HTTP management API with endpoints similar to:

```text
POST   /v1/applications
GET    /v1/applications
POST   /v1/streams
GET    /v1/streams
GET    /v1/streams/{id}
PATCH  /v1/streams/{id}
POST   /v1/streams/{id}/rotate-publish-key
POST   /v1/streams/{id}/playback-token
GET    /v1/streams/{id}/status
GET    /v1/streams/{id}/viewers
POST   /v1/streams/{id}/disconnect-publisher
POST   /v1/streams/{id}/disconnect-viewers
GET    /health/live
GET    /health/ready
GET    /metrics
```

## API requirements

* Versioned routes
* Authentication
* Authorisation
* JSON schema validation
* Structured errors
* Request IDs
* Audit logging
* Pagination
* Rate limiting
* No secret leakage
* Correct HTTP status codes
* Graceful database failure behaviour

## Persistence

1. Fix CMake SQLite target issues properly.
2. Create schema migrations.
3. Use transactions.
4. Add indexes.
5. Add unique constraints.
6. Document SQLite limitations.
7. Design repository interfaces so PostgreSQL can be added without rewriting media code.
8. Keep persistence interfaces outside RTMP protocol classes.

## Required tests

* Valid publisher authentication
* Invalid publisher key
* Rotated publisher key
* Expired playback token
* Modified playback token
* Disabled stream
* Disabled application
* Viewer limit
* Publisher limit
* Per-IP limit
* Token constant-time validation path
* Management API validation
* Database unavailable
* Configuration cache refresh
* No secret in logs

## Definition of done

* Publish and playback authentication are enforced by the actual RTMP path.
* Public names and secret keys map to internal stream IDs.
* Management API controls real server state.
* Media workers do not block on database operations.
* Secrets are not stored or logged insecurely.
* Readiness correctly reflects required dependencies.

Stop after Phase 5.

---

# PHASE 6 — Recording and optional HLS/LL-HLS output

## Objective

Support scalable HTTP-based viewer delivery while retaining RTMP output.

## Important architecture decision

RTMP may remain the ingest protocol:

```text
OBS → RTMP ingest
```

For browser and large-scale public playback, provide:

```text
RTMP ingest → HLS/LL-HLS packager → HTTP server/CDN
```

## Tasks

1. Audit existing FLV recording code.
2. Move recording disk I/O off network event-loop threads.
3. Add a bounded recording queue.
4. Define recording failure policy.
5. Rotate recording files safely.
6. Write temporary files and atomically finalise them where appropriate.
7. Add disk-space monitoring.
8. Add configurable recording retention.

## HLS tasks

When source codecs are compatible:

* H.264 video
* AAC audio

Implement passthrough packaging without transcoding.

Support:

* MPEG-TS HLS or CMAF/fMP4
* Media playlists
* Master playlists when multiple renditions exist
* Segment duration configuration
* Playlist window
* Discontinuity handling
* Codec header changes
* Timestamp rollover
* Publisher reconnect
* Atomic playlist updates
* Segment cleanup
* Cache-control headers
* Signed playback integration

Do not build a raw H.264/AAC encoder from scratch.

For adaptive bitrate transcoding, design a separate worker-process integration. Keep transcoding outside network workers.

## Required tests

* FLV recording start/stop
* Disk write failure
* Queue overflow
* Publisher disconnect
* HLS segment creation
* Playlist correctness
* Sequence header changes
* Timestamp discontinuity
* Segment cleanup
* Multiple viewers requesting segments
* Process restart behaviour
* No disk I/O on RTMP worker thread

## Definition of done

* Recording cannot block media workers.
* Recording memory usage is bounded.
* H.264/AAC passthrough streams can produce valid HLS output.
* Generated playlists are tested with a real player or validation tool.
* CDN-compatible HTTP behaviour is documented.

Stop after Phase 6.

---

# PHASE 7 — Observability, real load testing and capacity validation

## Objective

Prove server behaviour with realistic workloads.

## Metrics

Expose at least:

```text
active_connections
active_publishers
active_viewers
viewers_per_stream
connections_per_worker
ingress_bytes_total
egress_bytes_total
ingress_bitrate
egress_bitrate
outbound_queue_bytes
outbound_queue_packets
dropped_video_frames
dropped_audio_frames
slow_viewer_recoveries
slow_viewer_evictions
authentication_failures
partial_send_count
connection_timeouts
publisher_disconnects
viewer_disconnects
gop_cache_bytes
gop_cache_packets
inter_worker_queue_depth
inter_worker_queue_drops
io_uring_sq_full
io_uring_cq_overflow
provided_buffer_exhaustion
recording_queue_depth
recording_failures
process_memory_bytes
worker_cpu_usage
```

Avoid high-cardinality metric labels such as raw connection IDs.

## Logging

Implement structured logs containing:

* Timestamp
* Severity
* Component
* Worker ID
* Connection ID
* Stream ID
* Application ID
* Event
* Error code
* Latency
* Request ID where relevant

Never log:

* Full publish secret
* Full bearer token
* Sensitive query string
* Private credentials

## Real load generator

Replace or supplement the synthetic benchmark.

Build a realistic load tool capable of:

1. Opening real TCP connections.
2. Performing RTMP handshake.
3. Sending RTMP commands.
4. Publishing realistic H.264/AAC payload sizes.
5. Creating many RTMP viewers.
6. Configurable bitrate.
7. Configurable keyframe interval.
8. Configurable connection ramp-up.
9. Slow viewer simulation.
10. Abrupt disconnect simulation.
11. Publisher reconnect simulation.
12. Collecting latency and corruption statistics.

## Required scenarios

Run, subject to machine capacity:

```text
1 publisher + 100 viewers
1 publisher + 500 viewers
1 publisher + 1,000 viewers
10 publishers + 100 viewers each
Viewer connection burst
Slow viewers
Publisher reconnect
Network interruption
Large keyframes
24-hour soak test where practical
```

Do not claim the server supports 1,000 viewers merely because a synthetic in-memory benchmark loops over 1,000 subscribers.

## Capacity report

Create:

```text
docs/capacity-report.md
```

Include:

* Hardware
* CPU
* RAM
* NIC speed
* Kernel
* Compiler
* Build flags
* Source bitrate
* Viewer count
* Total egress bitrate
* CPU usage
* Memory usage
* Queue usage
* Frame drops
* Disconnects
* Test duration
* Bottleneck
* Safe recommended capacity

Clearly separate:

```text
Protocol capacity
CPU capacity
Memory capacity
Network bandwidth capacity
```

## Definition of done

* Metrics are available.
* Logs are structured.
* Real socket-based load tests exist.
* Load tests use realistic payload sizes.
* Slow viewers are tested.
* Capacity claims are backed by measured evidence.
* Safe operating limits are documented.

Stop after Phase 7.

---

# PHASE 8 — Security hardening, deployment and production release

## Objective

Prepare the server for safe production deployment.

## Security tasks

1. Add TLS strategy:

   * Native RTMPS
   * Or documented TLS proxy termination
2. Set secure protocol and cipher defaults.
3. Add malformed protocol tests.
4. Add fuzz targets for:

   * RTMP chunk decoder
   * AMF decoder
   * Handshake parser
   * Token parser
5. Add input size and recursion limits.
6. Validate all client-controlled lengths before allocation.
7. Add connection and authentication rate limits.
8. Review integer overflow risks.
9. Review timestamp rollover.
10. Review directory traversal risks in recording and HLS paths.
11. Run:

* ASan
* UBSan
* TSan where compatible
* Static analysis
* Compiler warnings at strict levels

## Deployment tasks

Create:

* Production CMake preset
* Debug CMake preset
* Sanitizer presets
* Release packaging
* systemd unit
* Environment file example
* Log rotation configuration
* Graceful shutdown behaviour
* Restart policy
* File descriptor limit guidance
* Kernel tuning guidance
* Network buffer guidance
* Health-check configuration
* Upgrade procedure
* Rollback procedure
* Backup procedure for persistence
* Secret rotation procedure

## Example production areas to document

```text
LimitNOFILE
systemd restart policy
TCP listen backlog
socket buffer sizing
ephemeral ports
NIC bandwidth
CPU affinity
core dumps
log retention
recording disk capacity
TLS certificates
firewall rules
management API exposure
```

Do not blindly apply kernel tuning values. Explain the reason and expected effect of every recommendation.

## Release gates

The release must fail when:

* Tests fail
* Sanitizers report errors
* Required configuration is missing
* Database migrations fail
* Unsupported insecure defaults are used
* Compiler warnings exceed the agreed policy

## Definition of done

* Production build is reproducible.
* systemd deployment is documented and tested.
* Graceful shutdown works.
* Security limits are enforced.
* Fuzz tests exist for exposed parsers.
* No known critical sanitizer issues remain.
* Rollback and upgrade procedures are documented.
* Final architecture documentation matches the code.

Stop after Phase 8.

---

# 6. Final production acceptance criteria

The project is not production-ready until all relevant criteria below are satisfied.

## Protocol

* Complete RTMP handshake
* Correct chunk decoding and encoding
* AMF command processing
* Publish and play lifecycle
* RTMP control messages
* Ping/pong
* Safe malformed-input handling

## Connection lifecycle

* Deterministic ownership
* Partial send support
* Bounded reads and writes
* Correct timeout handling
* Safe shutdown
* No use-after-free
* No double close
* No stale completion access

## Streaming

* Internal stream IDs
* Secure publish authentication
* Secure playback authentication
* Shared immutable media payloads
* Correct GOP cache
* Bounded viewer queues
* Slow-viewer recovery and eviction
* No callbacks while holding global fan-out locks

## Scalability

* Multiple `io_uring` workers
* Connection affinity
* Egress worker sharding
* Bounded cross-worker queues
* No per-viewer cross-thread media copies
* Realistic network load testing

## Operations

* Metrics
* Structured logs
* Liveness
* Readiness
* Management API
* Graceful shutdown
* systemd deployment
* Upgrade and rollback documentation

## Evidence

* Actual build logs
* Actual test results
* Sanitizer results
* Load-test results
* Capacity report
* Known limitations

---

# 7. Behaviour when uncertain

When information is missing:

1. Inspect the repository first.
2. Use the existing architecture and naming conventions.
3. Make the smallest safe assumption.
4. Document the assumption.
5. Do not stop for minor questions.
6. Ask only when a decision would create a major incompatible public API or data migration.

Do not replace working architecture merely because another design is more fashionable.

---

# 8. Coding-output requirements

When implementing changes:

* Show the exact files modified.
* Explain important lifetime and concurrency decisions.
* Provide complete code, not isolated pseudocode.
* Keep diffs focused on the current phase.
* Add comments explaining why, not restating what the code does.
* Update CMake for every new source and test.
* Update relevant documentation.
* Apply formatting consistently.
* Run the formatter only on changed files unless the repository explicitly requires otherwise.

---

# 9. Start instruction

Start with **PHASE 0 only**.

Do not implement Phase 1 yet.

First:

1. Inspect the complete repository.
2. Build it.
3. Run the existing tests.
4. Trace the real runtime architecture.
5. Verify every suspected issue.
6. Create the Phase 0 documentation.
7. Produce the Phase 0 completion report.
8. Stop and wait for:

```text
CONTINUE TO PHASE 1
```
