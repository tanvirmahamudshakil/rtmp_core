#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace rtmp_server::protocol::commands {

// Strongly-typed wrapper around a monotonically-allocated std::uint64_t.
// Deliberately has no implicit conversion to/from a raw integer and no
// conversion between distinct Tag types, so an ApplicationId can never be
// accidentally passed where a StreamId is expected (docs/v2_promot.md
// PHASE 3 "Replace raw subscriber pointers with safe handles" generalized
// to every identity this phase introduces, not just SubscriberId).
//
// Value 0 is reserved as "invalid/unset" (default-constructed); real IDs
// start at 1 via next().
template <typename Tag>
class StrongId {
public:
    constexpr StrongId() = default;

    // Mints a fresh, process-wide-unique ID for this Tag. Thread-safe.
    static StrongId next() { return StrongId(counter().fetch_add(1, std::memory_order_relaxed) + 1); }

    // Explicit escape hatches only — no implicit conversion operator, so
    // `raw()`/`from_raw()` must be called by name at every use site,
    // matching the "safe handle, not a raw pointer/int" intent.
    [[nodiscard]] std::uint64_t raw() const noexcept { return value_; }
    [[nodiscard]] static StrongId from_raw(std::uint64_t value) noexcept { return StrongId(value); }

    [[nodiscard]] bool valid() const noexcept { return value_ != 0; }
    explicit operator bool() const noexcept { return valid(); }

    friend bool operator==(const StrongId&, const StrongId&) noexcept = default;

private:
    explicit constexpr StrongId(std::uint64_t value) noexcept : value_(value) {}

    static std::atomic<std::uint64_t>& counter() {
        static std::atomic<std::uint64_t> instance{0};
        return instance;
    }

    std::uint64_t value_ = 0;
};

struct ApplicationIdTag {};
struct StreamIdTag {};
struct PublisherIdTag {};
struct SubscriberIdTag {};

using ApplicationId = StrongId<ApplicationIdTag>;
using StreamId = StrongId<StreamIdTag>;
using PublisherId = StrongId<PublisherIdTag>;
using SubscriberId = StrongId<SubscriberIdTag>;

// Resolves the durable mapping the doc requires:
//
//   Publish secret key -> authentication -> Internal StreamId
//                                                  ^
//                                          Public playback name
//
// Both the publisher (authenticated via the publish secret key) and every
// viewer (naming the stream by its public playback name) call resolve()
// with the same (app, stream_key) pair and get back the same StreamId —
// the doc's "both publisher and viewer must resolve to the same internal
// StreamId" requirement. A StreamId is minted once per (app, stream_key)
// and reused across republish under the same name (it is only removed via
// forget(), which callers use once a stream is torn down and its name is
// free to be reassigned to a fresh StreamId on next publish, matching
// "publisher replacement policy").
class StreamIdRegistry {
public:
    [[nodiscard]] StreamId resolve(std::string_view app, std::string_view stream_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = make_key(app, stream_key);
        auto it = keys_.find(key);
        if (it != keys_.end()) return it->second;
        StreamId id = StreamId::next();
        keys_.emplace(std::move(key), id);
        return id;
    }

    [[nodiscard]] std::optional<StreamId> find(std::string_view app, std::string_view stream_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = keys_.find(make_key(app, stream_key));
        if (it == keys_.end()) return std::nullopt;
        return it->second;
    }

    // Drops the (app, stream_key) -> StreamId mapping. A subsequent
    // resolve() for the same name mints a brand-new StreamId rather than
    // reusing the old one — used when a stream name is retired so a later
    // publisher under the same name is treated as a distinct stream
    // identity, not a silent resurrection of stale fan-out state.
    void forget(std::string_view app, std::string_view stream_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        keys_.erase(make_key(app, stream_key));
    }

private:
    static std::string make_key(std::string_view app, std::string_view stream_key) {
        std::string key;
        key.reserve(app.size() + stream_key.size() + 1);
        key.append(app);
        key.push_back('\x1f'); // ASCII unit separator: not a legal RTMP app/stream-key byte
        key.append(stream_key);
        return key;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StreamId> keys_;
};

} // namespace rtmp_server::protocol::commands
