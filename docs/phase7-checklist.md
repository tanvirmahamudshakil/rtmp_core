# Phase 7 Implementation Checklist — RTMP Playback

- [x] subscriber session — `CommandSession::handle_play` now validates a
  non-empty stream key, transitions the `NetStream` slot to `Playing`, and (if
  a `LiveFanout` is injected) creates a `CommandSession::PlaybackRelay` and
  subscribes it — one relay per `message_stream_id`, owned in
  `playback_relays_`
- [x] GOP cache — `LiveFanout` retains every audio/video message since the
  last video keyframe (single interleaved cache, nginx-rtmp-style), reset on
  each new keyframe, independent of whether anyone is subscribed
- [x] metadata startup — `LiveFanout` retains the latest `onMetaData` message
  and replays it first to a new subscriber
- [x] codec startup — `LiveFanout` retains the latest AVC (`AVCPacketType==0`)
  and AAC (`AACPacketType==0`) sequence headers and replays them before any
  cached GOP frames, so a new viewer's decoder is configured before any NALU
  arrives
- [x] live fan-out — `route_media_message` forwards every Audio/Video/
  Amf0Data message on a Publishing stream into `LiveFanout::on_audio/
  on_video/on_metadata`, which dispatches to every current subscriber
  synchronously
- [x] viewer backpressure — `CommandSession::deliver_playback_message` checks
  an injected `PendingBytesProvider` against `max_queued_playback_bytes_`
  (default 4 MiB) before emitting; over budget, the frame is dropped (`false`
  returned to `LiveFanout`) instead of queued — same bounded-queue,
  drop-newest policy as `recording::Recorder`
- [x] slow-client handling — `LiveFanout` counts each subscriber's consecutive
  dropped messages and force-unsubscribes (`on_slow_client_evicted`) once a
  configurable threshold (default 100) is crossed, so one backed-up viewer
  can never stall dispatch to everyone else or to the publisher
- [x] disconnect cleanup — publisher teardown (`deleteStream` or connection
  close) calls `LiveFanout::publisher_stopped`, which notifies every viewer
  (`on_publisher_stopped` → `NetStream.Play.UnpublishNotify`) and drops all
  cached state; viewer teardown unsubscribes its relay
- [x] tests — 7 new GoogleTest cases (6 `CommandSessionTest` + 1
  `LiveFanoutTest`), see below

## Files created

- `include/rtmp_server/protocol/commands/live_fanout.hpp` (`PlaybackSink`
  abstract hook + `LiveFanout` hub)
- `src/protocol/commands/live_fanout.cpp`
- `docs/rtmp-playback.md`
- `docs/phase7-checklist.md` (this file)

## Files changed

- `include/rtmp_server/protocol/commands/command_session.hpp` — added
  `#include "live_fanout.hpp"` and `<memory>`; `set_live_fanout()`,
  `set_pending_bytes_provider()`, `set_max_queued_playback_bytes()`;
  declared `~CommandSession()`/move constructor out-of-line (needed once
  `playback_relays_` held `unique_ptr<PlaybackRelay>` to an only-forward-
  declared nested type); private `PlaybackRelay`, `deliver_playback_message`,
  `handle_playback_publisher_stopped`, `handle_playback_evicted`
- `src/protocol/commands/command_session.cpp` — `PlaybackRelay` (implements
  `PlaybackSink`, forwards into the owning session); `handle_play` now
  subscribes to the fanout; `handle_delete_stream`/`on_connection_closed`
  handle the `Playing` branch (unsubscribe) alongside the existing
  `Publishing` branch (now also calls `publisher_stopped`);
  `route_media_message` also forwards to `live_fanout_`
- `src/protocol/CMakeLists.txt` — added `commands/live_fanout.cpp`
- `tests/protocol/commands/command_session_test.cpp` — added
  `OneViewerReceivesFannedOutMedia`, `MultipleViewersEachReceiveFannedOutMedia`,
  `NewViewerReceivesCachedGopAndSequenceHeaders`,
  `PublisherDisconnectEndsViewerSessionCleanly`,
  `ViewerDisconnectRemovesItFromFanoutSubscriberList`,
  `LiveFanoutTest.SlowClientIsEvictedAfterMaxConsecutiveDrops`

## Architecture decisions

- **LiveFanout is a shared hub, not a per-session sink.** Unlike
  `RecorderSink` (1 publisher : 1 recorder), playback is inherently 1
  publisher : N viewers, so the GOP cache and subscriber list must be
  reachable from every `CommandSession` involved — the same sharing model as
  `StreamRegistry` (constructed once, referenced by every session), not the
  private-pointer-per-session model `RecorderSink`/`MediaIngest` use.
  `stream_registry.hpp`'s own class doc anticipated this: "bitrate stats, GOP
  cache, subscriber lists ... belongs to those later phases, not [Stream
  Registry]."
- **`PlaybackSink` mirrors `RecorderSink`'s shape** (abstract hook in the
  protocol layer, non-owning, no exceptions) so the protocol layer's
  dependency graph stays acyclic, but adds a return `bool` (delivered/dropped)
  since, unlike recording, playback fan-out must let one slow viewer drop
  frames without affecting delivery to anyone else.
- **Backpressure decision lives with the sink, eviction lives with the hub.**
  `CommandSession::deliver_playback_message` is the only thing that knows this
  viewer's actual outbound backlog (via the injected `PendingBytesProvider`),
  so it alone decides per-message drop. `LiveFanout` only counts consecutive
  drops and evicts — it has no visibility into *why* a sink returned false,
  by design, so the policy for "how much backlog is too much" stays a
  transport-layer concern instead of leaking into the fan-out hub.
- **No callback re-entrancy into `LiveFanout`'s mutex.** `on_video`/`on_audio`/
  `on_metadata`/`subscribe`/`publisher_stopped` all copy what they need to
  call back into `PlaybackSink` methods, then release `mutex_` before calling
  them — otherwise a subscriber that reacted to `on_slow_client_evicted()` or
  `on_publisher_stopped()` by touching the fanout again would deadlock on the
  non-recursive `std::mutex`. The `PlaybackSink` contract documents that these
  two callbacks fire *after* the subscription is already gone, precisely so
  implementations have no reason to call back in.
- **Subscriber IDs are relay addresses, not an allocator.** Each `play()`
  creates one heap-owned `PlaybackRelay`; its address is stable for exactly
  the subscription's lifetime and is unique among concurrently-live
  subscribers without needing a shared ID counter across every
  `CommandSession` — same "address as key" trick used nowhere else in this
  codebase yet, but simple and correct given the ownership shape here.
- **`CommandSession` gained a user-declared destructor and move constructor.**
  `unordered_map<uint32_t, unique_ptr<PlaybackRelay>>` needs `PlaybackRelay`
  complete at the point its destructor is instantiated; `PlaybackRelay` is
  only forward-declared in the header (it's a `CommandSession`-private
  implementation detail defined in the .cpp), so the implicitly-defaulted
  special members (which would otherwise be generated wherever the class is
  first used, i.e. in the header) had to move out-of-line, defaulted in the
  .cpp where `PlaybackRelay` is complete. Copy is deleted (was already
  effectively impossible: `StreamRegistry&` is a reference member); move
  assignment is deleted for the same reason, move construction is kept
  (needed for `auto session = make_session();`-style factory functions, as in
  the existing test fixture).

## Build commands

```
$ cmake --preset core-only
$ cmake --build --preset core-only
```

## Test commands

```
$ ctest --preset core-only
```

## Actual test results

```
100% tests passed out of 132
Total Test time (real) =   1.32 sec
```

132 total: 125 pre-existing (Phase 0–6) + 7 new Phase 7 tests.

## Sanitizer results

```
$ cmake --preset asan
$ cmake --build --preset asan
$ ctest --preset asan
100% tests passed out of 132
Total Test time (real) =   6.19 sec
```

131/131 clean under ASan+UBSan, including the mutex-release-before-callback
paths and the pointer-cast-derived subscriber IDs (no use-after-free from
evicting/unsubscribing while iterating).

## Acceptance criteria evidence

- **one viewer plays** —
  `CommandSessionTest.OneViewerReceivesFannedOutMedia`: a viewer that
  subscribed before any media was published receives a live video frame
  published afterward, on its own `message_stream_id`.
- **multiple viewers play** —
  `CommandSessionTest.MultipleViewersEachReceiveFannedOutMedia`: three
  independent viewer sessions subscribe to the same key; a single published
  video frame reaches all three, each on its own `message_stream_id`.
- **new viewers receive cached GOP** —
  `CommandSessionTest.NewViewerReceivesCachedGopAndSequenceHeaders`: a viewer
  that subscribes *after* a sequence header + keyframe + interframe were
  already published receives, in order, the AVC sequence header then both
  cached video frames, without waiting for the next keyframe.
- **slow viewers do not block publisher** —
  `LiveFanoutTest.SlowClientIsEvictedAfterMaxConsecutiveDrops`: a subscriber
  whose sink always returns `false` (simulating a backlogged viewer) is
  force-unsubscribed after `max_consecutive_drops`; dispatch to it never
  blocks (`PlaybackSink::on_*` are synchronous, non-blocking calls by
  contract) and never throws, so publish-side `route_media_message` always
  returns promptly regardless of viewer health.
- **publisher disconnect ends viewer sessions cleanly** —
  `CommandSessionTest.PublisherDisconnectEndsViewerSessionCleanly`: closing
  the publisher's connection sends the viewer a `NetStream.Play.
  UnpublishNotify` status, transitions its slot out of `Playing`, and removes
  it from `LiveFanout`'s subscriber list.

## Known limitations

- Audio-only or metadata-only startup (no video track) is untested; GOP
  caching for audio is gated on `!gop_cache.empty()`, i.e. it only starts
  once a video keyframe has been seen, so an audio-only stream currently
  never populates the cache — acceptable for the target use case (video
  streams) but worth revisiting if audio-only publishing becomes a
  requirement.
- No `NetStream.Play.Reset` / `|RtmpSampleAccess` / seek / pause handling —
  out of scope per the phase spec (no seek/pause acceptance criterion).
- Not wired into `event_loop.cpp`/`IoUringEventLoop` — same standing gap as
  Phases 4-6: no real socket transport on this host (macOS, no io_uring). The
  `PendingBytesProvider` is designed to be backed by a real
  `TcpConnection`-side queued-bytes accessor once that wiring happens; no
  such accessor exists on `TcpConnection` yet, so production wiring would
  also need to add one.
- Eviction/backpressure thresholds (`max_consecutive_drops`,
  `max_queued_playback_bytes_`) are compile-time defaults with setters, not
  yet exposed through `core::Config`.

## Security concerns

- `play` with an empty stream key is rejected before touching `LiveFanout`
  (mirrors `publish`'s empty-key handling), so it can't create a subscription
  with an ambiguous key.
- No authorization check on `play` in this phase (any connected client can
  play any published key) — same posture as pre-Phase-4 `publish` before
  `StreamKeyValidator` was added; a playback-specific validator hook is a
  reasonable follow-up but wasn't part of this phase's acceptance criteria.
- `LiveFanout` never allocates unbounded memory from a single publisher: the
  GOP cache holds at most one GOP's worth of frames (cleared every keyframe);
  a pathological publisher that never sends a keyframe leaves the cache
  perpetually empty (frames pushed only once `gop_cache` already has content
  from a prior keyframe), not perpetually growing.
- A slow/malicious viewer cannot grow unbounded server memory or block other
  viewers/the publisher: it is auto-evicted after `max_consecutive_drops`,
  and per-message backpressure is a byte-budget check, not a queue.

## Performance concerns

- Steady-state fan-out is verbatim payload passthrough (the `RtmpMessage` is
  copied once per subscriber in `deliver_playback_message`, no re-encoding),
  same "no transcoding" posture as recording.
- `LiveFanout::mutex_` is held only for map/deque bookkeeping and the
  drop-count updates, never across a `PlaybackSink` call — so one slow
  viewer's callback can't hold up the next `on_video`/`on_audio` call for a
  different stream, or even the next dispatch iteration for other
  subscribers of the same stream (only excluded from re-entering the *same*
  hub instance's lock, not from being called).
- `subscribe()`'s cache replay happens once per subscription, bounded by one
  GOP's worth of frames plus 3 header messages — not proportional to stream
  uptime.

## Next phase

Next: Phase 8 — Management API and Link Generation (applications, streams,
stream creation, key generation, key rotation, publish/playback URLs, signed
tokens, enable/disable, disconnect controls, recording controls).
