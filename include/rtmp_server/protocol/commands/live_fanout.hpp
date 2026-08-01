#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/protocol/commands/gop_cache.hpp"
#include "rtmp_server/protocol/commands/shared_media_frame.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/viewer_queue.hpp"

namespace rtmp_server::protocol::commands {

// Abstract hook a viewer's CommandSession implements so LiveFanout can push
// media at it without depending on how the viewer actually delivers bytes.
//
// on_audio/on_video/on_metadata return false to mean transport backpressure
// prevented delivery. LiveFanout folds repeated false results into the
// slow-viewer eviction lifecycle after callbacks return; a backed-up socket
// therefore cannot remain subscribed forever while consuming fan-out CPU.
//
// Contract, unchanged from the prior revision: on_publisher_stopped() and
// on_slow_client_evicted() are called *after* LiveFanout has already
// removed the subscriber from its internal table, and NEVER while any
// LiveFanout mutex is held (see LiveFanout class doc). Implementations
// must not call LiveFanout::unsubscribe() from inside either callback.
class PlaybackSink {
public:
    virtual ~PlaybackSink() = default;

    virtual bool on_audio(const SharedMediaFrame& frame) = 0;
    virtual bool on_video(const SharedMediaFrame& frame) = 0;
    virtual bool on_metadata(const SharedMediaFrame& frame) = 0;

    // The publisher for the stream this sink was subscribed to has stopped
    // (deleteStream or disconnect). The subscription is already gone.
    virtual void on_publisher_stopped() = 0;

    // This subscriber fell far enough behind that the staged slow-viewer
    // policy gave up on it (ViewerQueue::Decision::Evict) and it has been
    // forcibly unsubscribed. The subscription is already gone.
    virtual void on_slow_client_evicted() = 0;
};

// Live fan-out hub (docs/v2_promot.md PHASE 3 "Stream identity, fan-out,
// GOP cache and backpressure"): keyed by the typed StreamId (not the
// publish-secret stream_key string — see stream_ids.hpp), one independently
// locked StreamState per stream ("per-stream locks" per the doc's fan-out
// tasks), immutable shared payloads (SharedMediaFrame, never deep-copied
// into the GOP cache or a viewer's accounting), a bounded GopCache, and a
// bounded, staged-policy ViewerQueue per subscriber.
//
// Thread-safety / "never call subscriber code while holding registry
// locks": every public method that can invoke a PlaybackSink method
// (on_audio/on_video/on_metadata/subscribe/publisher_stopped) follows the
// same two-phase shape:
//   1. Under the relevant per-stream StreamState::mutex (or, for structural
//      map operations, the outer streams_mutex_ — held only long enough to
//      look up/insert/erase a StreamState node, never across a callback):
//      mutate GOP cache / sequence headers / subscriber table, and decide
//      per-subscriber what to deliver via ViewerQueue's staged policy.
//   2. Outside any lock: actually invoke the collected PlaybackSink calls.
// This applies uniformly, unlike the pre-Phase-3 implementation which only
// deferred eviction notification outside the lock but called
// on_audio/on_video/on_metadata *while holding* the mutex.
class LiveFanout {
public:
    LiveFanout(GopLimits gop_limits = {}, QueueLimits queue_limits = {},
               std::size_t max_frames_waiting_for_keyframe = 250)
        : gop_limits_(gop_limits),
          queue_limits_(queue_limits),
          max_frames_waiting_for_keyframe_(max_frames_waiting_for_keyframe) {}

    // `is_replayed` is true when this frame arrived via the Phase 4
    // CrossWorkerRouter (i.e. it was ingested by a *different* worker's
    // LiveFanout and forwarded here so this worker's local subscribers can
    // see it). Replayed frames are dispatched through the exact same
    // keyframe/backpressure/sequence-header-cache path as locally-ingested
    // ones, but never re-invoke the forward hook — otherwise frames would
    // bounce endlessly between workers.
    void on_audio(StreamId stream_id, const SharedMediaFrame& frame, bool is_replayed = false);
    void on_video(StreamId stream_id, const SharedMediaFrame& frame, bool is_replayed = false);
    void on_metadata(StreamId stream_id, const SharedMediaFrame& frame, bool is_replayed = false);

    // Phase 4 multi-worker hooks (docs/v2_promot.md PHASE 4). Both are
    // optional; a single-worker embedder (or any existing Phase 1-3 test)
    // that never calls these simply gets no forwarding/subscription
    // notifications, unchanged behaviour otherwise.
    //
    // ForwardHook is invoked once per locally-ingested (non-replayed)
    // on_audio/on_video/on_metadata call, after local delivery has already
    // happened, with is_video/is_audio identifying the frame kind (both
    // false means metadata). The owning worker's CrossWorkerRouter uses this
    // to push the frame — once, not per-viewer — into every other worker's
    // inbound queue that currently has a subscriber for the stream.
    //
    // is_sticky marks a frame that is required decoder-init state a late
    // subscriber must have before any media decodes: onMetadata and the AVC/
    // AAC sequence headers. These are sent by the publisher once, before any
    // viewer on a *different* worker exists, so demand-gated forwarding would
    // never deliver them there and a cross-worker viewer would receive media
    // it cannot decode (video with no SPS/PPS). The router forwards sticky
    // frames to every worker unconditionally so each worker's LiveFanout
    // always holds the current init state for any future subscriber.
    using ForwardHook =
        std::function<void(StreamId stream_id, const SharedMediaFrame& frame, bool is_video, bool is_audio,
                           bool is_sticky, bool is_keyframe)>;
    void set_forward_hook(ForwardHook hook) { forward_hook_ = std::move(hook); }

    // SubscriptionHook is invoked with delta=+1 from subscribe() and
    // delta=-1 from unsubscribe() (only when a subscription actually
    // existed) and from eviction, so the router can maintain an accurate
    // per-worker subscriber count per stream without polling.
    using SubscriptionHook = std::function<void(StreamId stream_id, int delta)>;
    void set_subscription_hook(SubscriptionHook hook) { subscription_hook_ = std::move(hook); }

    // StreamEndHook is invoked from publisher_stopped() so the router can
    // drop its per-stream subscriber-count bookkeeping for a stream whose
    // cache was just discarded, rather than leaking stale entries.
    using StreamEndHook = std::function<void(StreamId stream_id)>;
    void set_stream_end_hook(StreamEndHook hook) { stream_end_hook_ = std::move(hook); }

    // Registers `sink` as a viewer of `stream_id` and immediately replays
    // cached startup state into it: metadata, then video sequence header,
    // then audio sequence header, then the cached GOP (if any) — "new
    // viewers receive metadata/audio seq header/video seq header/cached GOP
    // beginning at a keyframe" per the doc. If no keyframe has arrived yet,
    // only metadata/sequence headers are replayed (see GopCache class doc).
    // `subscriber_id` must be unique among concurrently-subscribed sinks
    // for this stream_id; not owned, caller must outlive the subscription
    // or unsubscribe first. `client_ip` (optional) is recorded only for
    // unique_viewer_count()'s dedup below — LiveFanout never uses it to
    // route or authorize anything.
    void subscribe(StreamId stream_id, SubscriberId subscriber_id, PlaybackSink* sink,
                    std::string_view client_ip = {});

    // Removes a subscription. Idempotent: safe to call even if never
    // subscribed, or already removed (e.g. via eviction or
    // publisher_stopped) — per docs/v2_promot.md PHASE 3 "Make unsubscribe
    // idempotent".
    void unsubscribe(StreamId stream_id, SubscriberId subscriber_id);

    // Publisher for `stream_id` stopped: notifies every current subscriber
    // via on_publisher_stopped() and discards all cached state (GOP cache,
    // sequence headers, metadata) for the id. A subsequent publish under a
    // freshly-resolved StreamId (see StreamIdRegistry::forget) starts with
    // a clean cache — "stream end cleanup".
    // is_replayed is set only when a CrossWorkerRouter StreamEnd control
    // reaches this worker. Replayed stops perform all local cleanup and
    // viewer notification but do not invoke stream_end_hook_ again, avoiding
    // an inter-worker rebroadcast loop.
    void publisher_stopped(StreamId stream_id, bool is_replayed = false);

    [[nodiscard]] std::size_t subscriber_count(StreamId stream_id) const;

    // Distinct client IPs currently subscribed to stream_id — the same
    // person reconnecting (reload, a second tab, a brief network hiccup
    // that reopens the RTMP session) counts once instead of once per
    // connection. A subscriber recorded with no client_ip (client_ip was
    // never supplied to subscribe()) always counts as its own unique
    // viewer, so callers that don't pass an IP see the old raw-count
    // behaviour rather than an undercount.
    [[nodiscard]] std::size_t unique_viewer_count(StreamId stream_id) const;

    // Cumulative bytes handed to viewer sinks for this stream since it first
    // appeared in streams_ (same bytes counted into the global
    // egress_bytes_total metric, just also kept per-stream). 0 for a stream
    // with no StreamState yet (never published/subscribed to). Callers poll
    // this and derive a bitrate from the delta over their own poll interval
    // — same pattern the admin panel already uses for viewer history, and it
    // avoids adding a clock dependency to LiveFanout just for this.
    [[nodiscard]] std::uint64_t egress_bytes_total(StreamId stream_id) const;

    // Phase 7 observability. Optional and non-owning; the embedder (server
    // main, or a test) owns the registry and must outlive this LiveFanout.
    // When null, every metric call site below compiles to a null check —
    // no behavioural change for the Phase 1-6 tests that never set it.
    //
    // What is recorded on the hot path (lock-free atomic adds only):
    //   dropped_video_frames / dropped_audio_frames  (ViewerQueue DropAndWait)
    //   slow_viewer_recoveries                       (DeliverResumed)
    //   slow_viewer_evictions                        (Evict)
    //   active_viewers                               (subscribe/unsubscribe)
    //   egress_bytes_total                           (bytes actually handed
    //                                                 to a PlaybackSink)
    // Gauges that require walking every stream (gop_cache_bytes,
    // outbound_queue_bytes, viewers_per_stream) are NOT maintained on the
    // hot path — see sample_gauges() below.
    void set_metrics(observability::Metrics* metrics) noexcept { metrics_ = metrics; }

    // Walks every live stream under its own per-stream lock and publishes the
    // aggregate gauges: gop_cache_bytes/packets, outbound_queue_bytes/packets
    // and the viewers_per_stream max/mean aggregation. Intended to be called
    // from a low-frequency sampler thread or from the /metrics handler, NOT
    // from the media path: it is O(streams + subscribers) and takes locks.
    // No PlaybackSink callback is invoked, so it cannot deadlock against
    // run_deliveries().
    void sample_gauges();

private:
    struct Subscriber {
        PlaybackSink* sink;
        ViewerQueue queue;
        std::size_t consecutive_transport_drops = 0;
        std::string client_ip; // empty when the caller didn't supply one
    };

    struct StreamState {
        explicit StreamState(GopLimits limits) : gop_cache(limits) {}

        std::mutex mutex;
        std::optional<SharedMediaFrame> metadata;
        std::optional<SharedMediaFrame> video_sequence_header;
        std::optional<SharedMediaFrame> audio_sequence_header;
        GopCache gop_cache;
        std::unordered_map<std::uint64_t, Subscriber> subscribers; // key = SubscriberId::raw()
        // Relaxed: only ever added to (run_deliveries) and read (egress_bytes_total),
        // never a synchronization point for anything else.
        std::atomic<std::uint64_t> egress_bytes_total{0};
    };

    // One pending, already-decided callback to run outside any lock.
    struct PendingDelivery {
        enum class Kind : std::uint8_t { Audio, Video, Metadata, Evict };
        std::uint64_t subscriber_id;
        PlaybackSink* sink;
        Kind kind;
        std::optional<SharedMediaFrame> resend_video_seq_header; // set only for DeliverResumed
        std::optional<SharedMediaFrame> resend_audio_seq_header; // set only for DeliverResumed
        // Non-owning pointer to the one immutable frame shared by this whole
        // dispatch. run_deliveries() is synchronous, so the caller's frame
        // outlives every callback. This avoids one shared_ptr refcount RMW per
        // viewer on the hottest fan-out path.
        const SharedMediaFrame* frame; // null for Evict
    };

    // Looks up (or creates) the StreamState node for `id`. The returned
    // shared ownership keeps the node alive after streams_mutex_ is released,
    // including across a concurrent publisher_stopped().
    std::shared_ptr<StreamState> state_for(StreamId id);

    // Shared implementation for on_audio/on_video/on_metadata: takes the
    // already-locked StreamState, decides per-subscriber delivery via each
    // ViewerQueue, evicts subscribers whose queue gives up on them, then
    // returns the deliveries to run outside the lock.
    // `stream_id` is used only to notify subscription_hook_ on eviction; the
    // hook is invoked while state.mutex is still held (it's a lightweight,
    // non-reentrant counter update, not a PlaybackSink callback, so this
    // doesn't violate the "never call subscriber code under lock" rule
    // above — see PlaybackSink's own doc comment).
    std::vector<PendingDelivery> dispatch_locked(StreamState& state, const SharedMediaFrame& frame, bool is_video,
                                                  bool is_audio, bool is_keyframe, StreamId stream_id);

    void run_deliveries(StreamId stream_id, const std::shared_ptr<StreamState>& state,
                        std::vector<PendingDelivery> deliveries);

    mutable std::mutex streams_mutex_; // guards only insert/erase of StreamState nodes in streams_
    std::unordered_map<std::uint64_t, std::shared_ptr<StreamState>> streams_; // key = StreamId::raw()
    GopLimits gop_limits_;
    QueueLimits queue_limits_;
    std::size_t max_frames_waiting_for_keyframe_;
    ForwardHook forward_hook_;
    SubscriptionHook subscription_hook_;
    StreamEndHook stream_end_hook_;
    observability::Metrics* metrics_ = nullptr; // not owned, may be null
};

} // namespace rtmp_server::protocol::commands
