#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

#include "rtmp_server/protocol/commands/shared_media_frame.hpp"

namespace rtmp_server::protocol::commands {

// Hard caps on how much of the most recent GOP LiveFanout retains,
// config-driven (core::ServerConfig::gop_cache_max_*), per docs/v2_promot.md
// PHASE 3 "Apply GOP limits".
struct GopLimits {
    std::uint64_t max_bytes = 16 * 1024 * 1024;
    std::uint32_t max_packets = 2000;
    std::chrono::milliseconds max_duration{10000};
};

// Retains audio+video SharedMediaFrame values (shared, never deep-copied)
// since the last video keyframe, nginx-rtmp style: reset on every new
// keyframe (begin_new_gop), not merely capped. If any of the configured
// byte/packet/duration limits is exceeded the cache is cleared entirely
// (not trimmed from the front) so playback always resumes at a clean
// keyframe boundary rather than a truncated, non-seekable GOP — "forces a
// keyframe-boundary restart" per docs/v2_promot.md PHASE 3 design.
//
// While no keyframe has arrived yet, has_gop() is false and push() is a
// no-op: new subscribers in that state get metadata + latest sequence
// headers only, and start receiving video once the first keyframe lands
// (docs/v2_promot.md PHASE 3 "Define behaviour when no keyframe has
// arrived").
class GopCache {
public:
    explicit GopCache(GopLimits limits = {}) : limits_(limits) {}

    void begin_new_gop(const SharedMediaFrame& keyframe) {
        clear();
        push_internal(keyframe);
    }

    // Appends a non-keyframe video or audio frame to the current GOP.
    // Returns false without modifying anything if no keyframe has started
    // a GOP yet (has_gop() == false) — callers must not call this before
    // the first begin_new_gop().
    bool push(const SharedMediaFrame& frame) {
        if (frames_.empty()) return false;
        push_internal(frame);
        enforce_limits();
        return true;
    }

    void clear() {
        frames_.clear();
        total_bytes_ = 0;
        start_timestamp_.reset();
    }

    [[nodiscard]] bool has_gop() const noexcept { return !frames_.empty(); }
    [[nodiscard]] const std::deque<SharedMediaFrame>& frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t total_bytes() const noexcept { return total_bytes_; }
    [[nodiscard]] std::size_t packet_count() const noexcept { return frames_.size(); }

private:
    void push_internal(const SharedMediaFrame& frame) {
        if (!start_timestamp_) start_timestamp_ = frame.timestamp;
        frames_.push_back(frame);
        total_bytes_ += frame.payload.size();
    }

    void enforce_limits() {
        bool over_bytes = limits_.max_bytes != 0 && total_bytes_ > limits_.max_bytes;
        bool over_packets = limits_.max_packets != 0 && frames_.size() > limits_.max_packets;
        bool over_duration = false;
        if (limits_.max_duration.count() != 0 && start_timestamp_ && !frames_.empty()) {
            std::uint32_t latest = frames_.back().timestamp;
            if (latest >= *start_timestamp_) {
                over_duration = std::chrono::milliseconds(latest - *start_timestamp_) > limits_.max_duration;
            }
        }
        if (over_bytes || over_packets || over_duration) clear();
    }

    GopLimits limits_;
    std::deque<SharedMediaFrame> frames_;
    std::uint64_t total_bytes_ = 0;
    std::optional<std::uint32_t> start_timestamp_;
};

} // namespace rtmp_server::protocol::commands
