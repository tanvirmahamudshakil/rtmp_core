#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "rtmp_server/core/buffer.hpp"

// MPEG-DASH segment types. Mirrors hls::Segment/SegmentPtr deliberately: the
// storage and delivery model (immutable, reference-counted, in-memory,
// bounded live window) is identical to HLS — only the container differs
// (fMP4/CMAF here instead of MPEG-TS).
namespace rtmp_server::dash {

// The `ftyp`+`moov` init segment for one representation. Built once per
// codec-configuration epoch (i.e. once the first SPS/PPS and
// AudioSpecificConfig arrive, and again if they change) and shared by every
// media segment that follows, exactly like an HLS EXT-X-MAP would be if HLS
// used one — DASH requires it up front, referenced by
// SegmentTemplate/initialization.
struct InitSegment {
    core::SharedBuffer data;
    // Bumped on every rebuild (a codec parameter change). A media segment
    // produced against an older epoch must not be served against a newer
    // init segment — see SegmentStore::add_segment.
    std::uint64_t epoch = 0;
};
using InitSegmentPtr = std::shared_ptr<const InitSegment>;

// One fMP4 media segment: a `styp` + one or more `moof`+`mdat` fragments
// (usually exactly one — the segmenter cuts on the same cadence as its HLS
// counterpart, one CMAF chunk per segment, no sub-segment fragmentation).
struct Segment {
    std::uint64_t number = 0;  // SegmentTemplate $Number$
    std::string name;          // e.g. "chunk-3.m4s"
    core::SharedBuffer data;
    std::chrono::milliseconds duration{0};
    // Epoch of the InitSegment this segment's samples were encoded against.
    std::uint64_t init_epoch = 0;

    [[nodiscard]] std::size_t size_bytes() const noexcept { return data.size(); }
};
using SegmentPtr = std::shared_ptr<const Segment>;

} // namespace rtmp_server::dash
