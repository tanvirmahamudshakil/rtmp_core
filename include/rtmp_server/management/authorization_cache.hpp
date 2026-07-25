#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "rtmp_server/core/clock.hpp"

namespace rtmp_server::management {

// TTL-bounded cache in front of a publish-key authorization check (Phase 9
// "authorization cache", docs/rtmp_promot.md "Persistence" — "Load
// authorization data into a bounded in-memory cache" to avoid a blocking
// database query on every publish attempt once StreamManager is
// persistence-backed). Wraps any bool(app, key) loader — in practice
// `StreamManager::validate_publish_key` — so the expensive path (hash
// compute + map/DB lookup) only runs once per (app, key) per `ttl`, not on
// every reconnect/retry a client makes.
//
// Deliberately not consulted on the RTMP media hot path (audio/video
// routing) — only at publish() time, which is connection-lifecycle-rate,
// not per-packet-rate, same scope every other Phase 9 addition targets.
class AuthorizationCache {
public:
    using Loader = std::function<bool(std::string_view application, std::string_view key)>;

    AuthorizationCache(Loader loader, std::chrono::milliseconds ttl, std::size_t max_entries = 10000)
        : loader_(std::move(loader)), ttl_(ttl), max_entries_(max_entries) {}

    // Returns the cached verdict if present and unexpired; otherwise calls
    // the loader, caches the result (even if false — caching negative
    // results is what protects against a probing attacker hammering unknown
    // keys, not just legitimate reconnects), and returns it.
    [[nodiscard]] bool authorize(std::string_view application, std::string_view key);

    // Drops every cached entry for (application, key) — call after a key
    // rotation or enable/disable change so the cache can't keep honoring a
    // just-revoked credential for up to `ttl` longer than intended.
    void invalidate(std::string_view application, std::string_view key);

    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        bool authorized;
        core::MonotonicClock::time_point expires_at;
    };

    [[nodiscard]] static std::string cache_key(std::string_view application, std::string_view key);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    Loader loader_;
    std::chrono::milliseconds ttl_;
    std::size_t max_entries_;
};

} // namespace rtmp_server::management
