# High-density RTMP delivery

The production RTMP path is optimized for one publisher feeding many direct
TCP viewers. It is still unicast: 1,000 viewers mean 1,000 sockets and roughly
1,000 times the source bitrate in outbound traffic.

## Hot path

For playback media, the server now:

1. keeps the ingested payload in one immutable `SharedMediaFrame`;
2. encodes a stateless, always-valid RTMP fmt0 wire buffer once per
   `(frame, chunk size, chunk-stream ID, message-stream ID)`;
3. serves the common cached wire representation through an immutable,
   lock-free read path and shares that allocation across every matching
   viewer queue, including viewers on other io_uring workers;
4. uses `IORING_OP_SEND_ZC` for buffers of at least 16 KiB when both config
   and the running kernel support it;
5. retains the source allocation through the SEND_ZC notification CQE, then
   releases it; and
6. disables zero-copy and retries with ordinary io_uring send if the
   kernel/NIC rejects the operation at runtime.

Small command/audio writes continue through ordinary io_uring send because
zero-copy setup and notification overhead is not worthwhile for small
buffers. Every connection still has byte and packet queue caps, and the
keyframe-aware slow-viewer policy prevents a slow socket from growing memory
without bound.

Cross-worker media queues signal their destination only when changing from
empty to non-empty. Further frames in the same burst reuse that wakeup, and a
bounded drain re-signals if work remains. This removes one `eventfd` syscall
per destination per frame without adding polling latency. Fan-out also reuses
its delivery records for post-callback backpressure results instead of
allocating a second vector for every frame.

The VPS installer sets a 4 MiB / 512-message slow-viewer queue. At common live
bitrates this absorbs several seconds of jitter while bounding worst-case RAM.
Varnish cache HITs remain unconstrained by origin concurrency; only concurrent
cache MISSes are capped to the origin's CPU-sized HTTP pool, preventing a
restart/join stampede from starving current playlists or media ingest.

Always-fmt0 media adds a few RTMP header bytes per frame. That deliberate
trade saves a payload-sized allocation/copy and stateful encode for every
viewer. Command and protocol-control traffic retains normal fmt0-3 header
compression on its connection-local chunk streams.

## Configuration

`enable_send_zero_copy: true` is capability-gated. It is safe to leave enabled
on a kernel without SEND_ZC: the server uses ordinary io_uring send.

The primary capacity controls remain:

- `maximum_connections`
- `maximum_viewers_per_stream`
- `worker_ring_count`
- `subscriber_queue_max_bytes`
- `subscriber_queue_max_packets`
- `provided_buffer_count`

Configured limits are safety ceilings, not performance claims.

## Required production acceptance test

Build and start the production server on the target Linux host. From a
separate Linux generator host:

```bash
SERVER_HOST=10.0.0.10 \
PLAYBACK_NAME='concert' \
APPLICATION='live' \
VIEWERS=1000 \
DURATION=300 \
bash scripts/load-test.sh
```

The load generator exits non-zero unless every requested publisher and viewer
reaches streaming, no client fails, and every verified media payload is
uncorrupted.

Repeat at the intended bitrate and viewer count, then run a 24-hour soak.
Monitor:

- link utilization and packet rate;
- `process_memory_bytes`;
- `outbound_queue_bytes`;
- `dropped_video_frames` / `dropped_audio_frames`;
- `slow_viewer_evictions`;
- `io_uring_sq_full` / `io_uring_cq_overflow`;
- `provided_buffer_exhaustion`; and
- `partial_send_count`.

Do not advertise a viewer capacity higher than the largest run that passes on
the actual server, NIC, kernel and network path. Direct RTMP capacity is
bounded first by:

```text
viewers <= usable outbound bits/second / total stream bits/second
```

The server does not transcode or reduce bitrate.
