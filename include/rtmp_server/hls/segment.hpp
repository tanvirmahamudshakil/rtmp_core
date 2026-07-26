#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "rtmp_server/core/buffer.hpp"

namespace rtmp_server::hls {

// One finished HLS media segment.
//
// `data` is a core::SharedBuffer (shared_ptr<const vector<byte>>), the same
// immutable-shared-storage pattern LiveFanout uses for media frames
// (docs/v2_promot.md 3.8). Every concurrent HTTP viewer requesting this
// segment receives a shared_ptr copy of the identical bytes — a segment is
// never deep-copied per viewer, no matter how many players fetch it.
struct Segment {
    std::uint64_t sequence = 0;               // EXT-X-MEDIA-SEQUENCE number
    std::string name;                          // e.g. "segment-42.ts"
    core::SharedBuffer data;
    std::chrono::milliseconds duration{0};
    // True when this segment does not continue the previous one's timeline
    // (codec header change, timestamp rollover, publisher reconnect). Drives
    // EXT-X-DISCONTINUITY in the playlist.
    bool discontinuity = false;

    [[nodiscard]] std::size_t size_bytes() const noexcept { return data.size(); }
};

using SegmentPtr = std::shared_ptr<const Segment>;

} // namespace rtmp_server::hls
