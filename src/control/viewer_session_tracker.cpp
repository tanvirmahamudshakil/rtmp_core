#include "rtmp_server/control/viewer_session_tracker.hpp"

namespace rtmp_server::control {

void ViewerSessionTracker::prune_locked(Shard& shard, std::chrono::steady_clock::time_point now) const {
    const auto cutoff = now - window_;
    for (auto it = shard.sessions.begin(); it != shard.sessions.end();) {
        if (it->second < cutoff) {
            it = shard.sessions.erase(it);
        } else {
            ++it;
        }
    }
}

void ViewerSessionTracker::record(std::string_view session, std::chrono::steady_clock::time_point now) {
    if (session.empty()) return;
    Shard& shard = shard_for(session);
    std::lock_guard lock(shard.mutex);
    // Deliberately constructs a std::string only on first sight of a session:
    // try_emplace leaves an existing entry alone, and the assignment below
    // refreshes its timestamp without touching the key.
    const auto [it, inserted] = shard.sessions.try_emplace(std::string(session), now);
    if (!inserted) it->second = now;
    // Sweep only once the shard is over its share of the cap. A shard at
    // steady state never pays for this.
    if (shard.sessions.size() > max_per_shard_) prune_locked(shard, now);
}

std::size_t ViewerSessionTracker::active_count(std::chrono::steady_clock::time_point now) {
    std::size_t total = 0;
    for (Shard& shard : shards_) {
        std::lock_guard lock(shard.mutex);
        prune_locked(shard, now);
        total += shard.sessions.size();
    }
    return total;
}

void ViewerSessionTracker::collect_active(std::unordered_set<std::string>& out,
                                          std::chrono::steady_clock::time_point now) {
    for (Shard& shard : shards_) {
        std::lock_guard lock(shard.mutex);
        prune_locked(shard, now);
        for (const auto& [session, seen_at] : shard.sessions) out.insert(session);
    }
}

} // namespace rtmp_server::control
