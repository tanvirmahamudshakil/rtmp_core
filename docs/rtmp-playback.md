# RTMP Playback (Phase 7)

## What this is

Phase 7 makes a published stream watchable: a client that sends `play
<stream-key>` on a `NetStream` gets the currently-cached codec setup and GOP
immediately, then every subsequent audio/video/metadata frame live, until
either side disconnects or the publisher stops.

Two new pieces, both in the protocol layer (`rtmp_server::protocol::commands`,
same layer as `CommandSession`/`StreamRegistry`, so this stays buildable and
testable under `RTMP_SERVER_CORE_ONLY` — no socket/io_uring dependency):

- **`PlaybackSink`** (`live_fanout.hpp`) — abstract hook a viewer implements,
  mirroring `RecorderSink`'s shape but for the 1:N playback direction.
- **`LiveFanout`** (`live_fanout.hpp`/`.cpp`) — the shared hub: GOP cache +
  subscriber list per stream key, fed by publishers, read by viewers.

## Why a separate hub instead of extending StreamRegistry or MediaIngest

`StreamRegistry` answers "who is publishing key K" — one row per key, no
per-key fan-out state. `MediaIngest` retains sequence headers and stats for
the *publisher's own* bookkeeping, not a byte-for-byte replay buffer, and it's
wired into `CommandSession` as a private, non-shared pointer (each session
that happens to reference the same `MediaIngest` instance could technically
work, but nothing about its API is designed for one-hub-many-readers).

Playback fan-out needs a genuinely different shape: one publisher's
`CommandSession::route_media_message` produces frames that N *different*
`CommandSession` instances (the viewers) must consume, live, in order, without
any one of them blocking the others or the publisher. That's a shared,
concurrently-read-and-written hub keyed by stream key — architecturally the
same sharing model as `StreamRegistry` (constructed once at the server level,
handed to every session by reference/pointer), just with a different, larger
payload (cached frames + subscriber callbacks instead of one registration
struct).

## GOP cache policy

`LiveFanout` retains, per stream key, independent of whether anyone is
currently watching:

- the latest `onMetaData` message
- the latest AVC sequence header (`AVCPacketType == 0`)
- the latest AAC sequence header (`AACPacketType == 0`)
- a single interleaved deque of every audio/video message since the last
  video keyframe (nginx-rtmp-style GOP cache)

Frame-type detection reads the FLV tag bytes directly (byte 0's nibbles for
frame type / codec / sound format, byte 1 for AVC/AAC packet type) — the same
convention `media::MediaIngest` already parses, just re-derived locally here
rather than reused, since `MediaIngest` doesn't retain the raw messages
`LiveFanout` needs to replay.

A video keyframe clears and restarts the cache; a sequence header is never
added to the cache (it's stored separately and always replayed first, so it
never goes stale relative to what's in the cache); audio frames only start
accumulating once the first keyframe has been seen, so a fresh subscriber's
replay is always keyframe-first.

## Startup sequence for a new subscriber

`LiveFanout::subscribe()` replays, synchronously, in this order:

1. cached `onMetaData` (if any)
2. cached AVC sequence header (if any)
3. cached AAC sequence header (if any)
4. every cached GOP frame, oldest first

This guarantees a decoder never sees a NALU/audio frame before its
configuration record, and never sees an inter-frame before the keyframe that
started its GOP — both hard requirements for a decoder to produce a picture
at all.

## Wiring into CommandSession

Same optional, non-owning injection pattern as `set_recorder`/
`set_media_ingest`:

```cpp
LiveFanout fanout;
publisher_session.set_live_fanout(&fanout);
viewer_session.set_live_fanout(&fanout);
```

`handle_play` creates one `CommandSession::PlaybackRelay` (a private
`PlaybackSink` implementation) per `play()`'d `message_stream_id` and
subscribes it. `route_media_message` forwards every Audio/Video/Amf0Data
message on a `Publishing` stream into the fanout, exactly parallel to how it
already forwards into `MediaIngest`/`RecorderSink`.

## Backpressure and slow-client handling

Two independent knobs, split by who has the information needed to use them:

- **`CommandSession::deliver_playback_message`** decides whether to actually
  emit a given frame to *this* viewer's `outgoing_handler_`, based on an
  injected `PendingBytesProvider` (meant to be backed by the viewer's real
  outbound socket queue depth once wired to a transport) checked against
  `max_queued_playback_bytes_` (default 4 MiB). Over budget → the frame is
  dropped, `false` is returned. This is the same bounded-queue, drop-newest
  policy `recording::Recorder` uses against disk backpressure, applied here
  against network backpressure.
- **`LiveFanout`** doesn't know or care *why* a sink returned `false` — it
  only counts consecutive failures per subscriber and force-unsubscribes
  (`on_slow_client_evicted()`) once `max_consecutive_drops` (default 100) is
  crossed. This bounds how long one backed-up viewer can keep occupying a
  slot in the subscriber map, and guarantees dispatch to every *other*
  subscriber, and the publisher's own ingest path, never blocks on it —
  `PlaybackSink` calls are synchronous but always return quickly (an
  in-memory check plus at most one queue push), never a blocking write.

## Disconnect cleanup

- **Publisher stops** (`deleteStream` or connection close while Publishing):
  `LiveFanout::publisher_stopped(stream_key)` notifies every subscriber
  (`on_publisher_stopped()` → the viewer's `CommandSession` sends
  `NetStream.Play.UnpublishNotify` and drops its slot back to `Idle`) and
  discards all cached state for that key, so a later republish starts clean.
- **Viewer stops** (`deleteStream` or connection close while Playing): the
  viewer's `CommandSession` unsubscribes its `PlaybackRelay` from
  `LiveFanout` and erases it.

Both paths are symmetric with how Phase 6 already tears down `RecorderSink`
on the publish side (`finalize()` on close) — teardown is always driven from
`CommandSession`'s own connection-close/deleteStream handling, never from a
timer or external reaper.

## Thread-safety and re-entrancy

`LiveFanout` guards its `streams_` map with one `std::mutex`. Every method
that must call back into a `PlaybackSink` (`on_video`/`on_audio`/
`on_metadata` for delivery + eviction, `subscribe` for the startup replay,
`publisher_stopped` for the stop notification) copies exactly what it needs
out from under the lock and releases it *before* invoking any `PlaybackSink`
method. This is required, not just tidy: `std::mutex` is non-recursive, and a
`CommandSession`'s `handle_playback_evicted`/`handle_playback_publisher_stopped`
callbacks run synchronously inside the call stack that originated in
`LiveFanout` — if the lock were still held, any future implementation detail
that called back into the same `LiveFanout` instance from inside those
callbacks would deadlock. The `PlaybackSink` contract documents that
`on_publisher_stopped()`/`on_slow_client_evicted()` fire *after* the
subscription is already removed, specifically so implementations have no
reason to call `unsubscribe()` again from inside them.

## Known limitations

See `docs/phase7-checklist.md` "Known limitations" — no audio-only GOP
caching, no seek/pause, and the standing not-wired-into-event_loop.cpp gap
every phase since Phase 4 has deferred.
