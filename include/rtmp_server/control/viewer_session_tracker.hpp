#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace rtmp_server::control {

// Distinct-playback-session counter for one stream key, written on the HLS
// delivery path and read by the periodic stats loop.
//
// Why sharded rather than one map under one mutex: every media-playlist and
// segment response calls record() once, so at N viewers this is touched
// N * (1 / segment_duration) times a second from whichever thread served the
// request. The previous single-mutex design serialised that against every
// other per-request field (request counters, the stream registry lookup),
// which put one global lock on the hot path of an otherwise parallel server.
// Sessions are independent, so they shard cleanly by hash: two viewers
// contend only when their session IDs collide into the same shard.
//
// Pruning is opportunistic, never a background thread. A shard is swept when
// a read visits it, and additionally on write once it exceeds its share of
// the session cap -- so a stream nobody reads stats for still cannot grow
// without bound, and the common path stays a single hash insert.
class ViewerSessionTracker {
public:
    // `window` is how long a session stays counted after its last request.
    // `max_sessions` bounds the total tracked across all shards; it exists to
    // cap memory against a flood of distinct, non-returning session IDs, not
    // to gate playback.
    ViewerSessionTracker(std::chrono::seconds window, std::size_t max_sessions) noexcept
        : window_(window),
          max_per_shard_(max_sessions / kShardCount > 0 ? max_sessions / kShardCount : 1) {}

    ViewerSessionTracker(const ViewerSessionTracker&) = delete;
    ViewerSessionTracker& operator=(const ViewerSessionTracker&) = delete;

    // Marks `session` as active as of `now`. Empty sessions are ignored: an
    // unidentified request still counts its bytes but cannot be a viewer.
    void record(std::string_view session, std::chrono::steady_clock::time_point now);

    // Number of sessions seen within the window, pruning expired ones.
    [[nodiscard]] std::size_t active_count(std::chrono::steady_clock::time_point now);

    // Inserts every still-active session ID into `out`, pruning expired ones.
    // Used to union sessions across an ABR ladder's renditions so one player
    // switching variants is never counted twice.
    void collect_active(std::unordered_set<std::string>& out,
                        std::chrono::steady_clock::time_point now);

private:
    // 64 shards keeps per-shard contention negligible at the connection
    // counts this server targets while costing ~64 mutexes per stream key --
    // trivial next to the session maps themselves.
    static constexpr std::size_t kShardCount = 64;

    struct Shard {
        std::mutex mutex;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> sessions;
    };

    [[nodiscard]] Shard& shard_for(std::string_view session) noexcept {
        return shards_[std::hash<std::string_view>{}(session) % kShardCount];
    }

    // Caller must hold `shard.mutex`.
    void prune_locked(Shard& shard, std::chrono::steady_clock::time_point now) const;

    std::chrono::seconds window_;
    std::size_t max_per_shard_;
    std::array<Shard, kShardCount> shards_;
};

} // namespace rtmp_server::control
