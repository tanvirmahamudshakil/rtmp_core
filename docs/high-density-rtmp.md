# High-density RTMP delivery

The production RTMP path is optimized for one publisher feeding many direct
TCP viewers. It is still unicast: 1,000 viewers mean 1,000 sockets and roughly
1,000 times the source bitrate in outbound traffic.

## Hot path

For playback media, the server now:

1. keeps the ingested payload in one immutable `SharedMediaFrame`;
2. encodes a stateless, always-valid RTMP fmt0 wire buffer once per
   `(frame, chunk size, chunk-stream ID, message-stream ID)`;
3. shares that immutable encoded allocation across every matching viewer
   queue, including viewers on other io_uring workers;
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
