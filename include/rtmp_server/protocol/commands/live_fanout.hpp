#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "rtmp_server/protocol/chunk/chunk_types.hpp"

namespace rtmp_server::protocol::commands {

// Abstract hook a viewer's CommandSession implements so LiveFanout can push
// media at it without depending on how the viewer actually delivers bytes
// (send over a real socket in production, capture into a vector in tests) —
// same non-owning-hook idea as RecorderSink, mirrored for the 1:N playback
// direction instead of publish's 1:1 direction.
//
// on_audio/on_video/on_metadata return false to mean "not delivered, this
// subscriber is backed up" (e.g. its outgoing byte budget is exceeded); they
// must never block or throw. LiveFanout counts consecutive false returns and
// auto-evicts a subscriber that stays backed up (see on_slow_client_evicted).
//
// Contract: on_publisher_stopped() and on_slow_client_evicted() are called
// *after* LiveFanout has already removed the subscriber from its internal
// table. Implementations must not call LiveFanout::unsubscribe() from inside
// either callback (it would be a redundant no-op at best; the intent is
// these are terminal notifications, not requests).
class PlaybackSink {
public:
    virtual ~PlaybackSink() = default;

    virtual bool on_audio(const chunk::RtmpMessage& message) = 0;
    virtual bool on_video(const chunk::RtmpMessage& message) = 0;
    virtual bool on_metadata(const chunk::RtmpMessage& message) = 0;

    // The publisher for the stream this sink was subscribed to has stopped
    // (deleteStream or disconnect). The subscription is already gone.
    virtual void on_publisher_stopped() = 0;

    // This subscriber missed max_consecutive_drops in a row and has been
    // forcibly unsubscribed. The subscription is already gone.
    virtual void on_slow_client_evicted() = 0;
};

// Live fan-out hub for RTMP Playback (Phase 7): keyed by stream_key exactly
// like StreamRegistry, but owns the parts StreamRegistry's class doc
// explicitly deferred to "later phases" — GOP cache and subscriber list
// (see stream_registry.hpp). Deliberately a separate, shared object (not a
// per-CommandSession member) because fan-out is inherently 1:N: one
// publisher's CommandSession feeds this hub, N viewer CommandSessions read
// from it, so it must outlive and be reachable from all of them — same
// sharing model as StreamRegistry.
//
// GOP cache policy: retains every audio/video message since the last video
// keyframe (nginx-rtmp-style single interleaved cache), plus the latest AVC/
// AAC sequence header and the latest onMetaData, all independent of whether
// anyone is currently subscribed. A new subscriber gets metadata, then the
// video sequence header, then the audio sequence header, then the GOP cache,
// synchronously inside subscribe() — "new viewers receive cached GOP"
// (docs/rtmp_promot.md Phase 7 acceptance criteria) without waiting for the
// next keyframe.
//
// Thread-safety: internally mutex-guarded like StreamRegistry. Never calls a
// PlaybackSink method while holding that mutex (copies out from under the
// lock instead) to avoid a self-deadlock via unsubscribe() re-entrancy from
// a callback — but implementations still must not call back into
// (un)subscribe() from inside on_publisher_stopped()/on_slow_client_evicted()
// per the PlaybackSink contract above.
class LiveFanout {
public:
    // A subscriber that fails to receive max_consecutive_drops messages in a
    // row (PlaybackSink::on_* returning false) is forcibly evicted — this is
    // the "slow-client handling" policy: a viewer that cannot keep up must
    // never be allowed to backpressure the shared, single-threaded fan-out
    // path that every other viewer and the publisher depend on.
    explicit LiveFanout(std::size_t max_consecutive_drops = 100) : max_consecutive_drops_(max_consecutive_drops) {}

    void on_audio(const std::string& stream_key, const chunk::RtmpMessage& message);
    void on_video(const std::string& stream_key, const chunk::RtmpMessage& message);
    void on_metadata(const std::string& stream_key, const chunk::RtmpMessage& message);

    // Registers `sink` as a viewer of `stream_key` and immediately replays
    // cached startup state into it (see class doc). `subscriber_id` must be
    // unique among concurrently-subscribed sinks for this stream_key; not
    // owned, caller must outlive the subscription or unsubscribe first.
    void subscribe(const std::string& stream_key, std::uint64_t subscriber_id, PlaybackSink* sink);

    // Removes a subscription. Safe to call even if never subscribed, or
    // already removed (e.g. via eviction or publisher_stopped).
    void unsubscribe(const std::string& stream_key, std::uint64_t subscriber_id);

    // Publisher for `stream_key` stopped: notifies every current subscriber
    // via on_publisher_stopped() and discards all cached state (GOP cache,
    // sequence headers, metadata) for the key. A subsequent republish starts
    // with a clean cache.
    void publisher_stopped(const std::string& stream_key);

    [[nodiscard]] std::size_t subscriber_count(const std::string& stream_key) const;

private:
    struct Subscriber {
        PlaybackSink* sink;
        std::size_t consecutive_drops = 0;
    };
    struct StreamState {
        std::optional<chunk::RtmpMessage> metadata;
        std::optional<chunk::RtmpMessage> video_sequence_header;
        std::optional<chunk::RtmpMessage> audio_sequence_header;
        std::deque<chunk::RtmpMessage> gop_cache;
        std::unordered_map<std::uint64_t, Subscriber> subscribers;
    };

    // Delivers `message` to every subscriber of `state` via `method`,
    // removes any subscriber that crosses max_consecutive_drops_ from
    // `state.subscribers`, and returns their sinks so the caller can invoke
    // on_slow_client_evicted() *after* releasing mutex_.
    std::vector<PlaybackSink*> dispatch_locked(StreamState& state, const chunk::RtmpMessage& message,
                                                bool (PlaybackSink::*method)(const chunk::RtmpMessage&));

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StreamState> streams_;
    std::size_t max_consecutive_drops_;
};

} // namespace rtmp_server::protocol::commands
