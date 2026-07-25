#include "rtmp_server/management/authorization_cache.hpp"

namespace rtmp_server::management {

std::string AuthorizationCache::cache_key(std::string_view application, std::string_view key) {
    std::string out;
    out.reserve(application.size() + 1 + key.size());
    out += application;
    out += '\0'; // NUL is not a legal app-name byte, so this can't collide
                 // ("app" + "\0key") vs. ("ap" + "\0p" + "key")
    out += key;
    return out;
}

bool AuthorizationCache::authorize(std::string_view application, std::string_view key) {
    std::string cache_key_str = cache_key(application, key);
    auto now = core::monotonic_now();

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(cache_key_str);
    if (it != entries_.end() && it->second.expires_at > now) {
        return it->second.authorized;
    }

    bool authorized = loader_(application, key);

    if (entries_.size() >= max_entries_ && it == entries_.end()) {
        // Bounded, not a real LRU: evicting an arbitrary entry under
        // sustained overflow just means slightly more loader_() calls, never
        // unbounded growth — same tradeoff LiveFanout's GOP cache and
        // AuditLog's ring buffer make in favor of a simple bound.
        entries_.erase(entries_.begin());
    }
    entries_[cache_key_str] = Entry{authorized, now + ttl_};
    return authorized;
}

void AuthorizationCache::invalidate(std::string_view application, std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(cache_key(application, key));
}

std::size_t AuthorizationCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

} // namespace rtmp_server::management
