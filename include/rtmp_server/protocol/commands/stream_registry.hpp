#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/clock.hpp"

namespace rtmp_server::protocol::commands {

// Minimal registry of currently-publishing streams, keyed by stream key
// (the last path segment of the publish URL, e.g. "publish foo" -> "foo").
// Deliberately transport-independent (connection_id is an opaque integer,
// not a network::TcpConnection) so it lives in the protocol layer next to
// CommandSession and stays testable/buildable under RTMP_SERVER_CORE_ONLY,
// matching how ChunkDecoder/ChunkEncoder/HandshakeSession have no socket
// dependency (docs/architecture.md "Architectural Separation").
//
// This is intentionally thin: Phase 4 only needs "is this key currently
// published, and by whom" so `publish` can enforce single-publisher-per-key
// and later phases (Media Ingest, Playback) have something to look a stream
// key up in. Anything beyond that (bitrate stats, GOP cache, subscriber
// lists) belongs to those later phases, not here.
struct StreamRegistration {
    std::string app;
    std::string stream_key;
    std::uint64_t connection_id = 0;
    std::uint32_t stream_id = 0;
    core::MonotonicClock::time_point start_time;
};

class StreamRegistry {
public:
    // Registers `key` as being published by `connection_id`/`stream_id`.
    // Returns false without modifying the registry if the key is already
    // published by a *different* connection (single-publisher-per-key
    // enforcement); re-registering the same connection_id+stream_key pair
    // (e.g. a republish) succeeds and refreshes the entry.
    bool register_publisher(std::string app, std::string stream_key, std::uint64_t connection_id,
                             std::uint32_t stream_id,
                             core::MonotonicClock::time_point start_time = core::monotonic_now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_key);
        if (it != streams_.end() && it->second.connection_id != connection_id) {
            return false;
        }
        StreamRegistration reg;
        reg.app = std::move(app);
        reg.stream_key = stream_key;
        reg.connection_id = connection_id;
        reg.stream_id = stream_id;
        reg.start_time = start_time;
        streams_[std::move(stream_key)] = std::move(reg);
        return true;
    }

    // Removes any registration held by `connection_id` for `stream_key`. A
    // no-op if that connection is not the current publisher (or nothing is
    // registered for the key at all).
    void unregister_publisher(std::string_view stream_key, std::uint64_t connection_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(std::string(stream_key));
        if (it != streams_.end() && it->second.connection_id == connection_id) {
            streams_.erase(it);
        }
    }

    // Removes every registration held by `connection_id`, regardless of
    // stream key — used on connection close/teardown.
    void unregister_all_for_connection(std::uint64_t connection_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = streams_.begin(); it != streams_.end();) {
            if (it->second.connection_id == connection_id) it = streams_.erase(it);
            else ++it;
        }
    }

    [[nodiscard]] bool is_published(std::string_view stream_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return streams_.contains(std::string(stream_key));
    }

    [[nodiscard]] std::optional<StreamRegistration> find(std::string_view stream_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(std::string(stream_key));
        if (it == streams_.end()) return std::nullopt;
        return it->second;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return streams_.size();
    }

    [[nodiscard]] std::vector<StreamRegistration> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<StreamRegistration> out;
        out.reserve(streams_.size());
        for (const auto& [key, reg] : streams_) out.push_back(reg);
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, StreamRegistration> streams_;
};

} // namespace rtmp_server::protocol::commands
