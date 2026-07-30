#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/hevc_params.hpp"

namespace rtmp_server::transcoding::native {

// One encoded HEVC access unit, Annex B framed (start-code prefixed), with the
// VPS/SPS/PPS prepended on keyframes (repeat_headers). Timestamps are on the
// 90 kHz clock, matching the TS muxer's expectations.
struct EncodedAccessUnit {
    std::vector<std::byte> annexb;
    std::int64_t pts_90k = 0;
    std::int64_t dts_90k = 0;
    bool keyframe = false;
};

// Wraps an x265 encoder. Fixed output geometry/frame-rate for the encoder's
// lifetime (a source resolution change tears down and rebuilds the pipeline).
// Because x265 buffers frames for lookahead/B-pyramid, encode() may emit zero
// access units for the first few inputs, then one per input; flush() drains the
// tail at end of stream.
class HevcEncoder {
public:
    HevcEncoder();
    ~HevcEncoder();
    HevcEncoder(const HevcEncoder&) = delete;
    HevcEncoder& operator=(const HevcEncoder&) = delete;

    [[nodiscard]] core::Result<void> open(const HevcParamSet& params);

    // Feeds one I420 frame. Appends any ready access units to `out`.
    [[nodiscard]] core::Result<void> encode(const YuvFrame& frame,
                                            std::vector<EncodedAccessUnit>& out);

    // Drains buffered frames at end of stream.
    [[nodiscard]] core::Result<void> flush(std::vector<EncodedAccessUnit>& out);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace rtmp_server::transcoding::native
