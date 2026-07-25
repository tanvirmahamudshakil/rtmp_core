# Production-Grade Raw C++ RTMP Streaming Server

## Role

You are a principal-level:

* C++ systems engineer
* Linux networking engineer
* `io_uring` specialist
* RTMP protocol engineer
* Streaming-media architect
* Security engineer
* Performance engineer
* DevOps engineer
* Test automation engineer

Your task is to inspect, design, implement, compile, test, document, and production-harden a professional RTMP streaming server written in modern C++.

---

# Project Location

The project folder is located at:

```text
<RTMP_PROJECT_PATH>
```

Examples:

```text
/home/user/rtmp
```

```text
D:\Projects\rtmp
```

```text
/Users/user/Projects/rtmp
```

Before writing any code, inspect this folder completely.

---

# Main Objective

Build a production-grade RTMP streaming platform that can:

1. Generate secure RTMP publishing links.
2. Generate RTMP playback links.
3. Accept RTMP input from OBS and other encoders.
4. Authenticate publishers using stream keys and signed tokens.
5. Receive H.264 video and AAC audio over RTMP.
6. Route one incoming live stream to multiple RTMP viewers.
7. Provide RTMP output for live playback.
8. Record incoming streams as valid FLV files.
9. Maintain live-stream status and statistics.
10. Expose a secure management API.
11. Allow full control over applications, streams, publishers, viewers, tokens, recordings, and limits.
12. Support graceful shutdown, reconnects, timeouts, rate limits, logging, metrics, testing, and deployment.
13. Provide extensible architecture for future HLS, LL-HLS, DASH, SRT, WebRTC, RTMPS, transcoding, clustering, CDN, and GPU acceleration.

This must not be:

* a toy server
* a single-file project
* a basic TCP echo server
* a fake demonstration
* a superficial proof of concept
* a project containing only empty interfaces and TODO comments

The result must be modular, testable, observable, secure, and maintainable.

---

# Mandatory Core Technologies

Use:

```text
Language: C++23
Operating system: Ubuntu Server 22.04 or later
Recommended kernel: Linux 6.x
Networking engine: Linux io_uring
Build system: CMake
Build tool: Ninja
Compiler: Clang and GCC
Debugger: GDB or LLDB
Testing: GoogleTest or Catch2
Production service: systemd
Containerization: Docker
```

The primary networking backend must use `io_uring`.

Do not use `epoll` as the main server backend.

---

# Dependency Rules

## Allowed dependencies

You may use:

* C++23 standard library
* Linux system APIs
* POSIX socket APIs
* `liburing`
* CMake
* Ninja
* Clang
* GCC
* OpenSSL for cryptography and future RTMPS
* SQLite for local development
* PostgreSQL for production persistence
* GoogleTest or Catch2 for tests
* a small, well-maintained JSON library for the management API
* a small configuration parser if clearly documented

## Forbidden dependencies

Do not use:

* FFmpeg
* libavcodec
* libavformat
* librtmp
* nginx-rtmp
* SRS source code
* Red5 source code
* Ant Media source code
* Wowza SDK
* GStreamer
* Live555
* Boost.Asio
* standalone Asio
* libuv
* existing RTMP server source code
* copied protocol implementations from external repositories
* one-thread-per-connection networking
* blocking sockets in the main server

The RTMP protocol must be implemented from protocol-level logic inside this project.

Do not copy existing RTMP server implementations.

---

# Important Media Scope

The initial server is a pass-through media server.

Implement:

* RTMP protocol
* H.264 packet inspection
* AAC packet inspection
* RTMP fan-out
* FLV recording
* stream routing
* authentication
* stream control

Do not falsely claim to implement:

* H.264 encoding
* H.265 encoding
* AV1 encoding
* AAC encoding
* software transcoding
* GPU transcoding
* media decoding

Codec decoding and transcoding must be separate future modules.

---

# Working Rules

Before changing the repository:

1. Inspect the entire project.
2. Print the existing file tree.
3. Read existing source files and build configuration.
4. Identify incomplete code and architectural conflicts.
5. Do not delete working code without explaining why.
6. Do not overwrite user code unnecessarily.
7. If the directory is empty, initialize a professional repository.
8. Create an implementation plan before coding.
9. Implement incrementally.
10. Compile after every meaningful phase.
11. Run tests after every meaningful phase.
12. Fix all compiler errors.
13. Fix serious compiler warnings.
14. Fix sanitizer failures.
15. Fix race conditions where discovered.
16. Do not claim success unless actual builds and tests pass.
17. Do not leave major functionality unimplemented while describing it as complete.
18. Clearly document unfinished features.
19. Never hide failing tests.
20. Preserve compatibility between phases.

Do not return only an architecture explanation.

Create actual project files, compile them, run tests, inspect failures, and fix them.

---

# High-Level Architecture

Use a layered architecture.

```text
                    +----------------------+
                    | Management API       |
                    | Streams / Keys / ACL |
                    +----------+-----------+
                               |
                               v
                    +----------------------+
                    | Stream Registry      |
                    | Auth / State / Stats |
                    +----------+-----------+
                               |
           +-------------------+-------------------+
           |                                       |
           v                                       v
+----------------------+                +----------------------+
| RTMP Publisher       |                | RTMP Subscribers     |
| OBS / Encoder Input  |                | Players / Relays     |
+----------+-----------+                +----------+-----------+
           |                                       ^
           v                                       |
+----------------------------------------------------------+
| RTMP Protocol Engine                                     |
| Handshake / Chunk / AMF0 / Commands / Media Messages     |
+--------------------------+-------------------------------+
                           |
                           v
+----------------------------------------------------------+
| Async Transport Layer                                    |
| Linux io_uring / TCP / Buffers / Timeouts / Cancellation |
+----------------------------------------------------------+
                           |
              +------------+------------+
              |                         |
              v                         v
       +--------------+          +--------------+
       | FLV Recorder |          | Metrics/Logs |
       +--------------+          +--------------+
```

---

# Architectural Separation

The RTMP protocol layer must remain independent from `io_uring`.

Only the transport layer may directly use:

* `io_uring`
* submission queue entries
* completion queue entries
* `liburing`
* registered buffers
* provided buffer rings
* asynchronous cancellation

Use an abstraction similar to:

```cpp
class IAsyncTransport {
public:
    virtual ~IAsyncTransport() = default;

    virtual void startRead() = 0;
    virtual void asyncWrite(SharedBuffer buffer) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const noexcept = 0;
};
```

The following layers must not directly call `liburing`:

* RTMP handshake
* chunk parser
* AMF0 parser
* RTMP command handler
* publisher logic
* subscriber logic
* stream registry
* GOP cache
* FLV writer
* authentication logic

---

# Required Repository Structure

Create or adapt the repository to a structure similar to:

```text
rtmp/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── LICENSE
├── .gitignore
├── .clang-format
├── .clang-tidy
├── cmake/
│   ├── CompilerWarnings.cmake
│   ├── Sanitizers.cmake
│   ├── StaticAnalysis.cmake
│   └── Dependencies.cmake
├── config/
│   ├── server.example.yaml
│   ├── logging.example.yaml
│   └── environment.example
├── docs/
│   ├── architecture.md
│   ├── io-uring-design.md
│   ├── connection-lifecycle.md
│   ├── buffer-ownership.md
│   ├── shutdown-model.md
│   ├── rtmp-protocol.md
│   ├── rtmp-handshake.md
│   ├── chunk-parser.md
│   ├── amf0.md
│   ├── stream-lifecycle.md
│   ├── timestamp-model.md
│   ├── security.md
│   ├── configuration.md
│   ├── control-api.md
│   ├── deployment.md
│   ├── testing.md
│   ├── troubleshooting.md
│   └── future-roadmap.md
├── include/
│   └── rtmp_server/
│       ├── core/
│       ├── io/
│       │   └── io_uring/
│       ├── network/
│       ├── protocol/
│       │   ├── handshake/
│       │   ├── chunk/
│       │   ├── amf0/
│       │   ├── commands/
│       │   └── messages/
│       ├── media/
│       │   ├── h264/
│       │   ├── aac/
│       │   ├── flv/
│       │   └── gop/
│       ├── server/
│       │   ├── connection/
│       │   ├── publisher/
│       │   ├── subscriber/
│       │   └── registry/
│       ├── authentication/
│       ├── recording/
│       ├── control/
│       ├── persistence/
│       └── observability/
├── src/
│   ├── core/
│   ├── io/
│   │   └── io_uring/
│   ├── network/
│   ├── protocol/
│   ├── media/
│   ├── server/
│   ├── authentication/
│   ├── recording/
│   ├── control/
│   ├── persistence/
│   └── observability/
├── apps/
│   ├── rtmp_server/
│   ├── rtmp_probe/
│   ├── rtmp_client_test/
│   └── flv_inspector/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── protocol/
│   ├── load/
│   └── fuzz/
├── scripts/
│   ├── build-debug.sh
│   ├── build-release.sh
│   ├── run-tests.sh
│   ├── run-sanitizers.sh
│   └── load-test.sh
└── deploy/
    ├── docker/
    ├── systemd/
    └── logrotate/
```

Adjust this structure only when technically justified.

---

# Core Foundation

Implement reusable core components.

## Error handling

Create:

* typed error codes
* result type
* operation status
* protocol errors
* network errors
* configuration errors
* authentication errors
* storage errors

Avoid using exceptions for expected packet-processing failures in hot paths.

Exceptions may be used for fatal startup errors where appropriate.

## Utility components

Implement:

* `ByteReader`
* `ByteWriter`
* big-endian read/write helpers
* 24-bit integer helpers
* monotonic clock
* wall clock
* timestamp helpers
* random identifier generator
* UUID generator or equivalent
* secure token generator
* secure random stream-key generator
* file descriptor RAII wrapper
* socket address abstraction
* signal handling
* cancellation state

## Buffer components

Implement:

```text
ByteBuffer
SharedBuffer
BufferSlice
BufferPool
RegisteredBufferPool
ProvidedBufferRing
PacketPool
```

Requirements:

* bounds-safe reads and writes
* no uncontrolled allocation per media packet
* bounded memory
* reference-counted immutable media payloads where useful
* no buffer reuse before operation completion
* clear ownership rules
* allocation-failure handling
* metrics for exhausted pools

---

# Linux io_uring Transport Layer

The main network engine must use `io_uring`.

## Required components

Create components similar to:

```text
IoUringContext
IoUringEventLoop
IoUringCapabilities
IoUringOperation
OperationRegistry
AsyncTcpAcceptor
AsyncTcpConnection
AsyncReadOperation
AsyncWriteOperation
AsyncTimeoutOperation
AsyncCancelOperation
RegisteredBufferPool
ProvidedBufferRing
ConnectionRegistry
```

## Operation types

Use explicit operation types:

```cpp
enum class OperationType : std::uint8_t {
    Accept,
    Receive,
    Send,
    Timeout,
    Cancel,
    Close,
    FileWrite,
    Wakeup,
    Shutdown
};
```

## Operation context

Every submitted operation must have a stable context.

Example:

```cpp
struct OperationContext {
    OperationType type;
    std::uint64_t operationId;
    std::uint64_t connectionId;
    std::uint64_t generation;
    std::weak_ptr<Connection> connection;
};
```

Do not identify operations using file descriptors alone.

File descriptors may be reused by the operating system.

Use:

* connection ID
* operation ID
* connection generation
* stable operation ownership

Do not put unsafe raw pointers into `user_data` unless their lifetime is formally guaranteed.

---

# io_uring Initialization

Use APIs such as:

```text
io_uring_queue_init_params
io_uring_get_sqe
io_uring_submit
io_uring_wait_cqe
io_uring_peek_batch_cqe
io_uring_cqe_seen
io_uring_queue_exit
```

The initialization process must:

1. Validate queue depth.
2. Initialize the ring.
3. Detect supported features.
4. Log capabilities.
5. Fail clearly if mandatory operations are unavailable.
6. Avoid silently falling back to blocking sockets.
7. Report memory requirements where practical.

---

# io_uring Capability Detection

At startup, detect support for:

* multishot accept
* multishot receive
* provided buffer rings
* registered buffers
* asynchronous cancellation
* linked timeouts
* zero-copy send
* required setup flags
* optional SQPOLL
* optional cooperative task running
* optional single issuer

Produce a startup report similar to:

```text
io_uring capabilities:
  multishot_accept: supported
  multishot_recv: supported
  provided_buffers: supported
  registered_buffers: supported
  async_cancel: supported
  linked_timeout: supported
  send_zero_copy: supported but disabled
  sqpoll: supported but disabled
```

Advanced feature absence must not crash the server.

Provide correct `io_uring` fallback operations.

Do not fall back to `epoll`.

---

# io_uring Event Loop

The event loop must:

1. Submit accept requests.
2. Submit receive requests.
3. Submit send requests.
4. Submit timeout requests.
5. Process completion batches.
6. Dispatch completions by operation type.
7. handle negative completion results.
8. correctly handle `IORING_CQE_F_MORE`.
9. re-submit recurring operations safely.
10. avoid busy spinning.
11. support wake-up commands.
12. support graceful shutdown.
13. drain outstanding completions.
14. release resources safely.
15. expose event-loop metrics.

Use batched completion processing where appropriate.

---

# Async Accept

Use asynchronous accept with `io_uring`.

Prefer multishot accept when supported.

Support:

* `IORING_OP_ACCEPT`
* multishot accept
* normal single-shot fallback

Requirements:

* detect multishot support
* inspect `IORING_CQE_F_MORE`
* submit a replacement accept when required
* use `SOCK_NONBLOCK`
* use `SOCK_CLOEXEC`
* configure accepted sockets
* enforce total connection limit
* enforce per-IP connection limit
* reject excess clients cleanly
* never lose the listener because of an accept error

Configure useful TCP options where justified:

* `TCP_NODELAY`
* `SO_KEEPALIVE`
* receive-buffer limits
* send-buffer limits

Do not blindly maximize OS socket buffers.

---

# Async Receive

Use `io_uring` receive operations.

Prefer provided buffer rings when supported.

Support:

* normal `IORING_OP_RECV`
* multishot receive where available
* provided buffer rings
* registered buffers where useful
* bounded receive accumulation
* fragmented TCP input
* multiple protocol messages per completion
* zero-length receive
* peer shutdown
* reset connections
* cancellation
* receive timeout

The receive system must correctly handle:

* RTMP C0 arriving alone
* C1 arriving in multiple parts
* C0 and C1 arriving together
* C2 arriving with RTMP command data
* multiple chunks in one network receive
* one RTMP chunk split across several receives

Do not assume one receive equals one RTMP message.

---

# Async Send

Implement ordered asynchronous writes.

Each RTMP connection must have one logical outbound byte stream.

Requirements:

* preserve RTMP message order
* preserve RTMP chunk order
* bounded write queue
* partial-send handling
* write offset tracking
* buffer lifetime until completion
* no concurrent unordered writes
* write timeout
* safe cancellation
* slow-client detection
* backpressure
* queue metrics

A valid design may use:

```text
Idle
  |
  v
Queue first buffer
  |
  v
Submit send
  |
  v
Receive completion
  |
  +-- partial --> submit remaining section
  |
  +-- complete --> pop queue and submit next
```

Do not allow multiple send operations to corrupt packet ordering.

## Optional zero-copy send

You may implement `IORING_OP_SEND_ZC` behind a disabled-by-default feature flag.

Requirements:

* detect kernel support
* retain normal-send fallback
* process notification completions correctly
* keep memory alive until notification completion
* do not enable by default until tests pass

---

# Timeout Management

Use `io_uring` timeouts for:

* RTMP handshake timeout
* authentication timeout
* idle connection timeout
* publisher inactivity timeout
* blocked-write timeout
* graceful shutdown deadline
* API request timeout
* recording flush timeout

Evaluate:

* `IORING_OP_TIMEOUT`
* `IORING_OP_LINK_TIMEOUT`
* `IORING_OP_TIMEOUT_REMOVE`

Every timeout must have:

* operation ID
* connection ID
* generation
* timeout purpose

Late timeout completions must not affect a new connection that reused the same file descriptor.

---

# Async Cancellation

Implement cancellation using `io_uring`.

Evaluate:

* `IORING_OP_ASYNC_CANCEL`
* cancellation by operation user data
* cancellation by file descriptor where appropriate

Connection shutdown sequence:

1. Mark connection as closing.
2. Reject new protocol operations.
3. Stop submitting receives.
4. Cancel active timeout operations.
5. Cancel pending receives.
6. handle send completion or cancellation safely.
7. process expected `-ECANCELED`.
8. close socket when safe.
9. remove connection from registry.
10. release buffers only after relevant completions.
11. destroy connection only when no operation can reference it.

Handle cancellation races:

* operation completed before cancellation
* cancellation completed first
* operation not found
* multiple operations belong to one connection
* connection already closing
* close completed before late completion

Expected cancellation results must not be logged as fatal errors.

---

# io_uring Error Classification

Handle negative completion values including:

```text
-ECANCELED
-EAGAIN
-EINTR
-ECONNRESET
-EPIPE
-ETIMEDOUT
-EBADF
-ENOENT
-ENOBUFS
-ENOMEM
-ENFILE
-EMFILE
```

Classify errors as:

* retryable
* remote disconnect
* expected cancellation
* timeout
* resource exhaustion
* protocol failure
* local configuration failure
* fatal server failure

Do not blindly retry all errors.

Prevent `SIGPIPE` from terminating the process.

---

# Threading Model

Begin with one `io_uring` event-loop thread.

After correctness is proven, support a configurable worker-ring count.

Possible architecture:

```text
                    Listener Socket
                           |
                           v
                  io_uring Accept Loop
                           |
           +---------------+---------------+
           |               |               |
           v               v               v
      Worker Ring 0   Worker Ring 1   Worker Ring N
```

Each connection must be owned by exactly one event loop.

Do not allow multiple event loops to mutate the same connection state.

Cross-thread commands must use:

* bounded queues
* explicit messages
* stable connection IDs
* an `eventfd` wake-up mechanism
* an `io_uring` poll operation on the eventfd

Document the chosen ownership model.

Do not introduce lock-free structures unless they are necessary, correct, benchmarked, and documented.

---

# RTMP Simple Handshake

Implement:

```text
Client -> Server: C0 + C1
Server -> Client: S0 + S1 + S2
Client -> Server: C2
```

Sizes:

```text
C0: 1 byte
C1: 1536 bytes
C2: 1536 bytes
S0: 1 byte
S1: 1536 bytes
S2: 1536 bytes
```

Use an explicit state machine:

```cpp
enum class HandshakeState {
    WaitingForC0,
    WaitingForC1,
    SendingS0S1S2,
    WaitingForC2,
    Completed,
    Failed
};
```

Requirements:

* fragmented reads
* combined reads
* invalid version rejection
* handshake timeout
* partial response writes
* connection-close cleanup
* no assumptions about TCP packet boundaries
* bounds checking
* unit tests
* integration test with an actual RTMP client or OBS

---

# RTMP Chunk Protocol

Implement raw RTMP chunk parsing and encoding.

Support:

* Basic Header
* chunk stream ID
* Message Header Type 0
* Message Header Type 1
* Message Header Type 2
* Message Header Type 3
* extended timestamp
* previous header state per chunk stream
* configurable input chunk size
* configurable output chunk size
* message reassembly
* interleaved chunk streams
* partial chunks
* multiple messages per receive
* maximum message-size validation
* malformed chunk rejection

Protocol-control messages:

* Set Chunk Size
* Abort Message
* Acknowledgement
* Window Acknowledgement Size
* Set Peer Bandwidth
* User Control Message

Implement acknowledgement-byte tracking.

---

# AMF0

Implement AMF0 reader and writer.

Support:

* Number
* Boolean
* String
* Long String
* Object
* Null
* Undefined
* ECMA Array
* Strict Array
* Object End

Security limits:

* maximum string length
* maximum nesting depth
* maximum object properties
* maximum array entries
* maximum decoded allocation
* malformed object termination handling

Unit-test every supported type.

---

# RTMP Command Handling

Implement publisher-side commands:

* `connect`
* `releaseStream`
* `FCPublish`
* `createStream`
* `publish`
* `deleteStream`
* `closeStream`

Implement playback-side commands:

* `connect`
* `createStream`
* `play`
* `pause`
* `deleteStream`
* `closeStream`

Implement responses:

* `_result`
* `_error`
* `onStatus`
* Stream Begin
* Stream EOF
* connection success
* publish start
* publish rejection
* play reset
* play start
* stream not found
* authentication failure

Support common OBS publishing behavior.

Do not assume one command arrives in one read or one chunk.

---

# RTMP Applications and Streams

Implement these concepts:

```text
Application
Stream
Stream ID
Stream name
Stream key
Publish token
Playback token
Token expiry
Enabled status
Recording policy
Publisher limit
Viewer limit
Allowed IP list
Denied IP list
Metadata
Created time
Updated time
```

Example application:

```text
live
```

Example stream:

```text
channel-001
```

Example internal identity:

```text
live/channel-001
```

---

# RTMP Link Generation

Generate secure publish and playback links.

## Publish URL

```text
rtmp://stream.example.com:1935/live/<stream-key>
```

## Playback URL

```text
rtmp://stream.example.com:1935/live/<stream-name>
```

## Signed publishing URL

```text
rtmp://stream.example.com:1935/live/<stream-name>?token=<signed-token>&expires=<unix-time>
```

## Signed playback URL

```text
rtmp://stream.example.com:1935/live/<stream-name>?token=<signed-token>&expires=<unix-time>
```

Security requirements:

* cryptographically secure keys
* constant-time secret comparison
* token expiration
* token revocation
* key rotation
* hashed key persistence where practical
* no key enumeration
* no secrets in logs
* no hardcoded secrets
* duplicate-publisher policy
* disabled-stream rejection

---

# Stream Registry

Implement a central stream registry.

Responsibilities:

* register publisher
* unregister publisher
* reject duplicate publisher
* optionally replace duplicate publisher
* add subscribers
* remove subscribers
* maintain stream state
* expose live stream list
* maintain publisher details
* maintain viewer count
* maintain byte counts
* maintain packet counts
* maintain bitrate
* maintain last activity
* clean up after disconnect

Avoid unnecessary global locking.

Use clear ownership and lifetime rules.

---

# Publisher Session

Publisher responsibilities:

* authentication
* stream registration
* receive media messages
* parse metadata
* parse H.264 headers
* parse AAC headers
* detect keyframes
* update statistics
* send packets to stream router
* write recording packets
* clean up on disconnect
* notify subscribers when source ends

Only one publisher should be accepted per stream unless configuration allows otherwise.

---

# Subscriber Session

Subscriber responsibilities:

* authenticate playback
* register with live stream
* receive metadata
* receive codec configuration
* receive latest GOP cache
* receive live packets
* maintain ordered send queue
* handle pause
* handle disconnect
* update viewer statistics
* apply backpressure
* disconnect when critically slow

A slow subscriber must never block the publisher.

---

# Media Packet Model

Create a transport-independent packet model.

Include:

```text
Packet type
Audio/video type
RTMP message type
DTS
PTS
Timestamp
Composition offset
Codec ID
Keyframe flag
Sequence-header flag
Stream ID
Source connection ID
Immutable payload buffer
```

Avoid copying large payloads unnecessarily.

Use shared immutable buffers where appropriate.

---

# H.264 Packet Inspection

Implement parsing for RTMP/FLV H.264 payloads.

Support:

* frame type
* codec ID
* AVC packet type
* AVC sequence header
* AVCDecoderConfigurationRecord
* NALU length size
* SPS
* PPS
* IDR keyframe detection
* composition-time offset
* DTS
* PTS

Do not decode video.

Validate all lengths before reading.

---

# AAC Packet Inspection

Implement parsing for RTMP/FLV AAC payloads.

Support:

* sound format
* sound rate
* sound size
* sound type
* AAC packet type
* AAC sequence header
* AudioSpecificConfig
* audio object type
* sample rate
* channel configuration
* AAC raw frame identification

Do not decode audio.

Validate all lengths before reading.

---

# Metadata

Handle relevant RTMP metadata, including `onMetaData`.

Extract where available:

* width
* height
* framerate
* video codec
* audio codec
* video bitrate
* audio bitrate
* sample rate
* audio channels
* encoder name

Store bounded metadata.

Reject unreasonable allocation sizes.

---

# GOP Cache

Implement a bounded GOP cache.

Cache:

* metadata
* video sequence header
* audio sequence header
* packets from the latest keyframe
* audio packets aligned with the current GOP

Limits:

* maximum byte size
* maximum packet count
* maximum duration
* configurable values

Requirements:

* send cache to new viewers
* support audio-only streams
* safely reset on codec-header changes
* safely reset on timestamp discontinuity
* prevent unbounded memory growth
* share packet payloads where possible

---

# RTMP Live Playback

Implement RTMP output.

When a viewer starts:

1. Validate the stream.
2. Send stream-control events.
3. Send metadata.
4. Send video sequence header.
5. Send audio sequence header.
6. Send cached GOP.
7. Start sending current live packets.

Requirements:

* correct timestamps
* correct output chunking
* configurable output chunk size
* bounded viewer queues
* partial send handling
* slow-client handling
* disconnect cleanup
* publisher-end notification

---

# Backpressure Policy

Implement explicit backpressure.

Example policy:

```text
Healthy queue:
  Send normally.

Warning threshold:
  Drop obsolete non-key video packets where safe.

Critical threshold:
  Clear packets until latest decodable keyframe where possible.

Persistent critical state:
  Disconnect subscriber.
```

Do not drop:

* codec configuration required for decoding
* metadata required for startup
* packets in a way that permanently corrupts subscriber state

Document exact behavior.

---

# FLV Recording

Implement valid FLV recording.

Support:

* FLV signature
* version
* audio/video flags
* data offset
* previous tag size
* script-data tags
* audio tags
* video tags
* 24-bit timestamps
* extended timestamp byte
* previous tag-size calculation

Requirements:

* configurable recording directory
* safe filename generation
* filename sanitization
* temporary extension while active
* atomic rename when finalized
* recording size limit
* disk-space checks
* disk-write failure handling
* start and stop events
* no blocking disk I/O in media hot path
* bounded recording queue

Create an FLV inspector tool to validate:

* header
* tag types
* timestamps
* tag sizes
* sequence headers
* keyframe positions

---

# Asynchronous Recording with io_uring

Use `io_uring` file-write operations where practical.

Requirements:

* ordered writes
* explicit file offsets
* bounded write queue
* partial-write handling
* file-write error handling
* cancellation during shutdown
* flush/finalize process
* disk-full handling
* maximum file-size handling

Do not let recording disk latency block publisher network processing.

When the recording queue is full, apply a documented policy.

---

# Authentication

Implement publisher and viewer authentication.

Support:

* stream keys
* API-generated keys
* signed tokens
* expiry timestamps
* IP restrictions
* enabled/disabled streams
* token revocation
* optional single-use tokens
* optional application-level secret
* constant-time comparisons
* failed-authentication throttling

Do not log full tokens or stream keys.

---

# Management API

Implement a secure HTTP management API in C++.

Required endpoints:

```text
POST   /api/v1/applications
GET    /api/v1/applications
GET    /api/v1/applications/{id}
PATCH  /api/v1/applications/{id}
DELETE /api/v1/applications/{id}

POST   /api/v1/streams
GET    /api/v1/streams
GET    /api/v1/streams/{id}
PATCH  /api/v1/streams/{id}
DELETE /api/v1/streams/{id}

POST   /api/v1/streams/{id}/rotate-key
POST   /api/v1/streams/{id}/enable
POST   /api/v1/streams/{id}/disable
POST   /api/v1/streams/{id}/disconnect-publisher
POST   /api/v1/streams/{id}/disconnect-viewers
POST   /api/v1/streams/{id}/start-recording
POST   /api/v1/streams/{id}/stop-recording

GET    /api/v1/live-streams
GET    /api/v1/live-streams/{application}/{stream}
GET    /api/v1/health
GET    /api/v1/ready
GET    /api/v1/metrics
```

Protect all management operations with authentication.

Do not bind the management API publicly by default.

Recommended default:

```text
127.0.0.1:8080
```

---

# Stream Creation Response

Example response:

```json
{
  "id": "d03d5ba9-35d9-4d88-b92f-762f6942a269",
  "application": "live",
  "stream_name": "channel-001",
  "stream_key": "generated-secure-key",
  "publish_url": "rtmp://stream.example.com:1935/live/generated-secure-key",
  "playback_url": "rtmp://stream.example.com:1935/live/channel-001",
  "enabled": true,
  "recording_enabled": false,
  "publisher_limit": 1,
  "viewer_limit": 1000,
  "expires_at": null,
  "created_at": "2026-07-25T00:00:00Z"
}
```

Only return the raw stream key:

* during creation
* during explicit key rotation

Do not include raw keys in ordinary list responses.

---

# Persistence

Create a persistence abstraction.

Development:

```text
SQLite
```

Production:

```text
PostgreSQL
```

Persist:

* applications
* streams
* hashed stream keys
* token revocations
* stream policies
* recording policies
* audit events
* created and updated timestamps

Do not perform blocking database queries for every media packet.

Load authorization data into a bounded in-memory cache.

Implement safe invalidation and refresh.

---

# Configuration

Support:

* YAML or TOML configuration file
* environment variables
* command-line arguments

Required configuration:

```text
rtmp_bind_address
rtmp_port
api_bind_address
api_port
public_rtmp_hostname

ring_queue_depth
completion_batch_size
submission_batch_size
worker_ring_count

enable_multishot_accept
enable_multishot_recv
enable_registered_buffers
enable_provided_buffer_ring
enable_send_zero_copy
enable_sqpoll
sqpoll_idle_ms

registered_buffer_count
registered_buffer_size
provided_buffer_count
provided_buffer_size

maximum_connections
maximum_connections_per_ip
maximum_publishers
maximum_viewers_per_stream

input_chunk_size
output_chunk_size
maximum_rtmp_message_size

handshake_timeout
authentication_timeout
idle_timeout
write_timeout
publisher_inactivity_timeout

gop_cache_max_duration
gop_cache_max_bytes
gop_cache_max_packets

subscriber_queue_max_bytes
subscriber_queue_max_packets

recording_directory
recording_enabled
recording_max_size
recording_queue_max_bytes

database_type
database_connection

token_signing_secret
api_authentication_secret

log_level
metrics_enabled
```

Validate configuration before startup.

Fail fast with clear errors.

Never silently accept dangerous default secrets.

---

# Structured Logging

Implement structured logging.

Useful fields:

```text
timestamp
level
component
event
connection_id
operation_id
remote_ip
application
stream_name
session_role
result
error_code
latency_ms
bytes
packets
```

Never log:

* raw stream keys
* full tokens
* database passwords
* API secrets
* TLS private keys

Do not log every media packet at production log levels.

---

# Metrics

Expose metrics for:

## Server metrics

* active TCP connections
* total accepted connections
* rejected connections
* current publishers
* current subscribers
* active streams
* authentication failures
* handshake failures
* malformed RTMP messages

## Network metrics

* incoming bytes
* outgoing bytes
* receive operations
* send operations
* connection resets
* timeouts
* write-queue sizes

## io_uring metrics

* SQE exhaustion
* CQE batch size
* active operations
* pending accepts
* pending receives
* pending sends
* pending timeouts
* pending cancellations
* stale completions
* ring-full events
* buffer exhaustion
* completion latency
* event-loop lag

## Media metrics

* audio packets
* video packets
* keyframes
* dropped packets
* stream bitrate
* subscriber queue drops
* slow-client disconnects
* GOP cache size

## Recording metrics

* active recordings
* recording bytes
* recording queue size
* file-write failures
* disk-full events

---

# Security Requirements

Implement:

* handshake-size limits
* RTMP message-size limits
* AMF string-size limits
* AMF nesting limits
* object-property limits
* connection limits
* per-IP limits
* authentication throttling
* invalid-key throttling
* idle timeout
* handshake timeout
* write timeout
* safe path handling
* filename sanitization
* integer-overflow checks
* parser bounds checks
* constant-time secret comparison
* secure random key generation
* audit logs
* API authentication
* IP allowlist and denylist
* duplicate-publisher protection
* slow-client protection
* bounded queues
* bounded buffers
* safe resource cleanup

Malformed clients must not crash the server.

---

# Connection Lifecycle

Document and implement explicit states.

Example:

```cpp
enum class ConnectionState {
    Accepted,
    Handshaking,
    Connected,
    Authenticating,
    Publishing,
    Playing,
    Closing,
    Closed,
    Failed
};
```

Define valid state transitions.

Reject invalid command sequences.

Examples:

* publish before connect
* play before createStream
* media packet before successful publish
* multiple publish commands on one stream ID
* command after close

---

# Timestamp Model

Document timestamp handling before implementing media routing.

Cover:

* RTMP message timestamps
* extended timestamps
* 32-bit timestamp wraparound
* DTS
* PTS
* composition-time offset
* publisher reconnect
* timestamp reset
* backward timestamp jumps
* FLV timestamps
* subscriber-relative timestamps
* sequence-header timestamps
* discontinuities

Do not implement timestamp logic through scattered ad hoc calculations.

Create a central timestamp normalization component.

---

# Testing Requirements

## Unit tests

Create tests for:

* byte reader
* byte writer
* endian helpers
* 24-bit integer helpers
* buffer pool
* operation IDs
* secure random keys
* token signing
* token validation
* configuration validation
* URL generation
* handshake parser
* chunk basic header
* header types 0–3
* extended timestamp
* chunk reassembly
* AMF0 types
* stream registry
* duplicate publisher rules
* H.264 sequence header
* AAC sequence header
* GOP cache
* FLV header
* FLV tags
* backpressure policy

## io_uring integration tests

Test:

* ring initialization
* ring shutdown
* asynchronous accept
* repeated connection
* asynchronous receive
* fragmented receive
* asynchronous send
* partial send
* remote reset
* timeout
* cancellation
* cancellation race
* stale completion
* buffer recycling
* buffer exhaustion
* connection limit
* graceful shutdown
* active-client shutdown
* active-recording shutdown
* high reconnect rate

## RTMP integration tests

Test:

* complete handshake
* fragmented handshake
* connect command
* createStream
* valid publish
* invalid key
* expired token
* duplicate publisher
* H.264/AAC media ingest
* metadata ingest
* one publisher and one viewer
* one publisher and multiple viewers
* viewer disconnect
* publisher disconnect
* reconnect
* FLV recording

## Robustness tests

Test:

* malformed handshake
* invalid chunk stream ID
* oversized RTMP message
* invalid AMF object
* excessive AMF nesting
* invalid media-packet lengths
* timestamp jump
* timestamp wraparound
* audio-only stream
* video-only stream
* codec-header change
* slow subscriber
* recording disk failure
* abrupt network disconnect
* long-running stream

## Fuzz tests

Create fuzz targets for:

* handshake parser
* chunk parser
* AMF0 parser
* FLV tag parser
* H.264 packet parser
* AAC packet parser

---

# Sanitizers and Static Analysis

Create build presets for:

```text
Debug
Release
ASan
UBSan
ASan+UBSan
TSan
Coverage
Fuzzing
```

Use compiler warnings:

```text
-Wall
-Wextra
-Wpedantic
-Wconversion
-Wshadow
-Wsign-conversion
-Wformat=2
-Wundef
-Wnull-dereference
-Wdouble-promotion
```

Only disable a warning when justified and documented.

Integrate:

* `clang-format`
* `clang-tidy`
* optional `cppcheck`

Do not claim production readiness while sanitizer failures remain.

---

# Performance Requirements

Design for:

* non-blocking networking
* bounded memory
* minimal packet copies
* reusable buffers
* batched completions
* batched submissions
* bounded send queues
* no database work in packet hot paths
* no file I/O blocking media routing
* one event-loop owner per connection
* clear cross-thread messages
* observable queue latency
* measurable event-loop latency

Do not optimize blindly.

Correctness comes first.

Measure before introducing complex optimizations.

---

# Load Testing

Create a load-test plan and tooling.

Test:

* idle connections
* concurrent publishers
* concurrent viewers
* repeated reconnects
* slow clients
* high packet rate
* long-running streams
* recording enabled
* recording disabled
* one large stream
* many small streams

Measure:

* CPU usage
* memory usage
* network throughput
* event-loop latency
* completion latency
* dropped packets
* viewer queue sizes
* connection failures
* recording latency

---

# Health Checks

Implement:

```text
GET /api/v1/health
```

This should report process liveness.

Implement:

```text
GET /api/v1/ready
```

This should verify:

* io_uring initialized
* listener active
* management API active
* database reachable where required
* configuration valid
* critical buffer pools available
* server not shutting down

---

# Graceful Shutdown

On `SIGTERM` or `SIGINT`:

1. Mark server as shutting down.
2. Stop accepting new clients.
3. Reject new management mutations.
4. Notify or close publishers.
5. notify or close viewers.
6. stop new recordings.
7. flush recording queues.
8. cancel timeouts.
9. cancel pending receives.
10. complete or cancel sends.
11. drain completion queues.
12. close sockets.
13. finalize recording files.
14. flush logs.
15. close database connections.
16. destroy rings.
17. exit with correct status.

Apply a configurable shutdown deadline.

Do not destroy operation contexts while completions can still arrive.

---

# Deployment

Create:

* `Dockerfile`
* `.dockerignore`
* `docker-compose.example.yml`
* systemd service
* logrotate configuration
* example environment file
* production configuration template
* health-check script
* installation script
* upgrade documentation
* rollback documentation

The server should run as:

```bash
./rtmp_server --config ./config/server.yaml
```

Systemd should run it as a non-root user.

Do not run the server as root unless technically required and documented.

---

# Example OBS Configuration

```text
Service: Custom
Server: rtmp://SERVER_IP:1935/live
Stream Key: GENERATED_STREAM_KEY
```

Example:

```text
Server: rtmp://192.0.2.10:1935/live
Stream Key: a-secure-generated-key
```

---

# Example Playback URL

```text
rtmp://SERVER_IP:1935/live/STREAM_NAME
```

Example:

```text
rtmp://192.0.2.10:1935/live/channel-001
```

---

# Documentation Requirements

Create:

```text
README.md
docs/architecture.md
docs/io-uring-design.md
docs/connection-lifecycle.md
docs/buffer-ownership.md
docs/shutdown-model.md
docs/rtmp-protocol.md
docs/rtmp-handshake.md
docs/chunk-parser.md
docs/amf0.md
docs/stream-lifecycle.md
docs/timestamp-model.md
docs/security.md
docs/configuration.md
docs/control-api.md
docs/deployment.md
docs/testing.md
docs/troubleshooting.md
docs/future-roadmap.md
```

The README must contain:

* project purpose
* architecture summary
* supported features
* unsupported features
* dependencies
* Ubuntu installation steps
* build steps
* run steps
* configuration
* OBS setup
* playback setup
* API examples
* testing
* sanitizer commands
* Docker deployment
* systemd deployment
* security notes
* troubleshooting
* future roadmap

---

# Future Extension Interfaces

Design clean extension points for:

```text
RTMPS
Enhanced RTMP
HLS
LL-HLS
CMAF
MPEG-DASH
SRT ingest
WHIP ingest
WebRTC playback
Transcoding workers
NVIDIA NVENC
NVIDIA NVDEC
Multi-node clustering
Origin and edge nodes
CDN integration
Object storage
Webhook events
Tenant billing
```

Possible interfaces:

```cpp
class IProtocolConnection;
class IAsyncTransport;
class IMediaSource;
class IMediaSink;
class IStreamAuthenticator;
class IStreamRegistry;
class IRecorder;
class IControlRepository;
class IMetricsSink;
class IEventPublisher;
class ITranscoderWorker;
```

Do not create empty abstractions without a current or clearly documented future purpose.

---

# Implementation Phases

## Phase 0: Repository Inspection and Design

Perform:

* inspect project folder
* print file tree
* inspect build files
* inspect existing source
* identify conflicts
* define module boundaries
* define connection ownership
* define operation ownership
* define buffer ownership
* define threading model
* define shutdown model
* define timestamp model
* define security boundaries
* create implementation checklist

Required output:

* existing repository assessment
* proposed architecture
* proposed file tree
* risks
* implementation order

Do not begin by deleting existing code.

---

## Phase 1: Core and io_uring TCP Foundation

Implement:

* CMake
* CMake presets
* C++23
* compiler warnings
* sanitizer builds
* logger
* configuration
* file descriptor RAII
* byte buffers
* bounded buffer pool
* io_uring capability detection
* io_uring context
* event loop
* asynchronous accept
* asynchronous receive
* ordered asynchronous send
* partial-send recovery
* timeout operations
* cancellation
* connection registry
* graceful shutdown
* tests

Acceptance criteria:

* listens on port 1935
* multiple clients connect
* receives fragmented data
* sends ordered output
* repeated connect/disconnect succeeds
* timeout works
* cancellation works
* graceful shutdown works
* ASan passes
* UBSan passes
* no `epoll` primary path exists

---

## Phase 2: RTMP Handshake

Implement:

* C0/C1/C2
* S0/S1/S2
* handshake state machine
* fragmented input
* partial output
* timeout
* invalid-version handling
* tests

Acceptance criteria:

* OBS reaches completed RTMP handshake
* fragmented handshake succeeds
* invalid handshake is rejected
* timeout closes stalled clients
* sanitizer builds pass

---

## Phase 3: RTMP Chunk Engine

Implement:

* chunk decoder
* chunk encoder
* message-header formats
* extended timestamps
* input/output chunk size
* protocol-control messages
* acknowledgement tracking
* tests

Acceptance criteria:

* types 0–3 pass
* partial chunks pass
* interleaved streams pass
* extended timestamps pass
* oversized messages are rejected

---

## Phase 4: AMF0 and RTMP Commands

Implement:

* AMF0
* connect
* releaseStream
* FCPublish
* createStream
* publish
* play
* close/delete stream
* result/status responses

Acceptance criteria:

* OBS receives connection success
* OBS creates a stream
* valid key can publish
* invalid key is rejected
* stream appears in registry

---

## Phase 5: Media Ingest

Implement:

* RTMP audio messages
* RTMP video messages
* metadata
* H.264 sequence-header parsing
* AAC sequence-header parsing
* keyframe detection
* timestamps
* statistics

Acceptance criteria:

* OBS publishes H.264/AAC
* metadata is parsed
* SPS/PPS are retained
* AAC configuration is retained
* keyframes are detected
* malformed packets are rejected

---

## Phase 6: FLV Recording

Implement:

* FLV header
* metadata tags
* audio tags
* video tags
* async file writer
* finalization
* FLV inspector
* tests

Acceptance criteria:

* recorded FLV is playable
* timestamps remain valid
* abrupt publisher disconnect finalizes safely
* recording queue is bounded
* disk failures do not crash server

---

## Phase 7: RTMP Playback

Implement:

* subscriber session
* GOP cache
* metadata startup
* codec startup
* live fan-out
* viewer backpressure
* slow-client handling
* disconnect cleanup

Acceptance criteria:

* one viewer plays
* multiple viewers play
* new viewers receive cached GOP
* slow viewers do not block publisher
* publisher disconnect ends viewer sessions cleanly

---

## Phase 8: Management API and Link Generation

Implement:

* applications
* streams
* stream creation
* key generation
* key rotation
* publish/playback URLs
* signed tokens
* enable/disable
* disconnect controls
* recording controls
* live-state endpoints
* API security

Acceptance criteria:

* API creates streams
* API returns secure URLs
* key rotation works
* token expiry works
* disabled stream is rejected
* publisher can be disconnected by API
* viewer sessions can be disconnected

---

## Phase 9: Persistence and Production Hardening

Implement:

* SQLite development persistence
* PostgreSQL production persistence
* authorization cache
* audit logs
* metrics
* Docker
* systemd
* fuzzing
* load testing
* static analysis
* production documentation

Acceptance criteria:

* clean release build
* all tests pass
* sanitizer builds pass
* startup validation works
* systemd deployment works
* Docker deployment works
* security review findings are documented

---

# Required Report After Every Phase

After each phase, report:

1. Files created.
2. Files changed.
3. Architecture decisions.
4. Build commands.
5. Test commands.
6. Actual test results.
7. Sanitizer results.
8. Known limitations.
9. Security concerns.
10. Performance concerns.
11. Next phase.

Do not report a test as passing unless it was actually executed.

---

# Error Resolution Rules

When a failure occurs:

1. Show the actual error.
2. Identify the root cause.
3. Fix the root cause.
4. Rebuild.
5. Rerun affected tests.
6. Confirm the result.
7. Document any remaining limitation.

Do not hide, bypass, disable, or comment out failing tests merely to obtain a green build.

Do not suppress compiler warnings without justification.

---

# First Milestone

The first executable milestone must be:

> A professional Linux C++23 TCP server using io_uring, listening on port 1935, with asynchronous accept, receive, ordered send, partial-write recovery, timeout handling, cancellation, bounded buffers, structured logging, configuration, graceful shutdown, tests, and sanitizer support.

After this milestone works, immediately implement the RTMP simple handshake.

---

# Start Now

Begin with the following actions:

1. Inspect `<RTMP_PROJECT_PATH>`.
2. Print the current repository tree.
3. Read all existing relevant files.
4. Explain the current repository state.
5. Create the Phase 0 architecture documents.
6. Create an implementation checklist.
7. Implement Phase 1.
8. Compile the project.
9. Run unit and integration tests.
10. Run ASan and UBSan.
11. Fix actual errors.
12. Report actual results.
13. Proceed to Phase 2 only after Phase 1 is stable.

Do not respond with only recommendations or sample snippets.

Work directly inside the project and create a real, compilable, tested implementation.
