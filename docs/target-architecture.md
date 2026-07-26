# Target Architecture

This is the architecture described by `docs/v2_promot.md` section 1 and the Phase 1-8 task
lists. It is aspirational relative to `docs/current-architecture.md` — nothing in this
document should be read as already implemented. It exists so later phases have a fixed
reference point to build toward incrementally, per the master prompt's "improve
incrementally, do not rewrite blindly" rule.

## 1. End-to-end flow

```
OBS / RTMP Encoder
        |
        v
RTMP Ingest Server
        |
        +-- Publish authentication (secret key -> internal StreamId, hashed key storage)
        +-- RTMP handshake (trailing bytes preserved into chunk decoder)
        +-- Chunk decoding (ChunkDecoder wired to real socket input)
        +-- AMF command processing (CommandSession wired to ChunkDecoder output)
        +-- Media ingest (MediaIngest wired to CommandSession audio/video/data)
        +-- Stream registry (StreamId <- publish key AND playback name both resolve here)
        +-- GOP cache (bounded: bytes/duration/packets)
        +-- Shared media frames (std::shared_ptr<const SharedBuffer>, no per-viewer deep copy)
                |
                +-- RTMP egress worker shards (N io_uring workers, connection-affinity)
                |       -> RTMP viewers (bounded per-viewer queue, slow-viewer policy)
                |
                +-- FLV recording (off event-loop thread, bounded queue, atomic finalize)
                |
                +-- Optional HLS/LL-HLS packaging (passthrough, no transcoding)
                        -> HTTP/CDN viewers
```

## 2. Connection lifecycle (Phase 1-2 target)

```
TcpConnection
    v
HandshakeSession        -- trailing post-C2 bytes handed to ChunkDecoder, not dropped
    v
RtmpConnectionSession    -- new abstraction; owns the rest for this connection's lifetime
    +-- ChunkDecoder     -- socket input -> RtmpMessage, enforces max message size
    +-- ChunkEncoder     -- RtmpMessage -> socket output, real vectored/partial-send-aware writes
    +-- CommandSession    -- connect/createStream/publish/play/deleteStream/FCUnpublish
    +-- Control message handler -- Set Chunk Size / Window Ack Size / Set Peer BW / Ack / User Control / Ping
    +-- Media ingest handler
    +-- Connection lifecycle handler -- deterministic teardown, generation-safe timeouts
```

Ownership: event loop (or worker) owns `TcpConnection` via the connection registry;
`RtmpConnectionSession` is reachable from the connection but does not outlive it;
`CommandSession`/publisher/subscriber state is torn down synchronously on connection close so
no late kernel completion can touch freed session memory (stable connection IDs + operation
generation IDs prevent stale-completion use-after-free, per section 3.4/Phase 2).

## 3. Transport correctness (Phase 2 target)

* Write queue entries carry buffer + current offset + remaining bytes + operation id +
  cancellation state; a queue entry is not popped until every byte is confirmed sent (fixes
  the current "any success pops the whole buffer" bug).
* EAGAIN/EINTR/EPIPE/ECONNRESET/ECANCELED/zero-byte completions are all handled explicitly.
* Configurable read/write/idle/handshake timeouts, generation-checked so a timeout cannot act
  on a reused connection object/fd.
* Graceful close: stop accepting writes, optionally flush a bounded amount, cancel outstanding
  ops, release resources only after their completions drain.

## 4. Stream identity, fan-out, backpressure (Phase 3 target)

```
struct ApplicationId; struct StreamId; struct PublisherId; struct SubscriberId;

Publish secret key --auth--> internal StreamId <--lookup-- Public playback name
```

* Per-stream ownership/locks (or worker-owned shards) replace the single global
  `LiveFanout::mutex_`.
* Subscriber callbacks are invoked only after copying out target handles under the lock and
  releasing it (the "correct pattern" from section 3.7) — the current code invokes
  `PlaybackSink::on_video/on_audio/on_metadata` while still holding `LiveFanout::mutex_`
  (`src/protocol/commands/live_fanout.cpp:55-69`), which must change.
* Media payload becomes `SharedMediaFrame{ StreamId, MediaType, timestamp, is_keyframe,
  is_sequence_header, std::shared_ptr<const SharedBuffer> payload }` instead of
  `RtmpMessage{ ..., std::vector<std::byte> payload }` copied per subscriber
  (`include/rtmp_server/protocol/chunk/chunk_types.hpp:47-53` today).
* Bounded GOP cache (bytes/duration/packets) and bounded per-viewer queue (bytes/packets),
  with the documented slow-viewer policy: drop non-key video -> wait for keyframe -> resume
  from keyframe with fresh sequence headers -> disconnect if still behind. Audio backlog
  policy explicitly bounded, not indefinite.

## 5. Multi-core io_uring workers (Phase 4 target)

```
Main process
    +-- Acceptor (central accept+dispatch, or SO_REUSEPORT -- to be decided in Phase 4)
    +-- Worker 0..N: own io_uring, connection map, timers, receive buffers, write queues
```

`worker_ring_count` actually creates N worker threads/rings; connection affinity is fixed to
one worker for its lifetime; a shared media frame is routed once per target egress worker
(not once per viewer) via bounded inter-worker queues; multishot accept/recv, provided
buffers, registered buffers are used where the kernel supports them and safely fall back
where it does not (`IoUringCapabilities` already exists and can back this — it is simply
unconsulted today).

## 6. Persistence / authentication / management API (Phase 5 target)

* Publish keys hashed at rest, never logged; rotation supported without invalidating
  outstanding playback tokens for the previous key's `StreamId` mapping.
* `CommandSession::handle_play` validates a signed playback token (audience: application +
  StreamId + expiry + optional session id/IP) with constant-time signature comparison,
  using `management::Token` (implemented and tested today, just not called from the RTMP
  path).
* A real HTTP management API (`/v1/applications`, `/v1/streams`, `/v1/streams/{id}/...`,
  `/health/live`, `/health/ready`, `/metrics`) backed by `StreamManager`, with authN/authZ,
  JSON schema validation, structured errors, request IDs, audit logging, pagination, rate
  limiting.
* Media workers never block on SQLite; hot-path authorization reads an in-memory
  immutable snapshot (`AuthorizationCache` already exists for this purpose) refreshed off
  the network threads.

## 7. Recording / HLS (Phase 6 target)

Recording disk I/O fully off network threads (the `Recorder` class's bounded-queue design
already supports this; it just needs to be driven by real media instead of test-injected
frames). Optional passthrough HLS/LL-HLS packaging for H.264/AAC without a from-scratch
encoder.

## 8. Observability / load testing (Phase 7 target)

Real metrics registry exposed over HTTP (`Metrics` class exists, has no endpoint today),
structured logs with connection/stream/application/worker IDs, and a real socket-based load
generator that performs actual TCP handshakes/RTMP publish/play with realistic H.264/AAC
frame sizes — replacing (or supplementing) `apps/load_bench`'s in-process synthetic loop.

## 9. Security / deployment (Phase 8 target)

TLS strategy (native RTMPS or documented proxy termination), fuzz targets for handshake/AMF/
chunk/token parsers (3 of 4 already exist under `fuzz/`; a handshake or command-session fuzz
target and token/query-parser fuzz target are the missing pieces), CMake presets for
debug/release/sanitizer builds, systemd unit (present in `deploy/systemd`, untested against a
server that can actually stay up under load), documented upgrade/rollback/secret-rotation
procedures.
