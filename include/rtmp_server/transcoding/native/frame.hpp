#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rtmp_server::transcoding::native {

// A planar 8-bit I420 (YUV 4:2:0) picture. This is the single intermediate
// representation between decode, scale and encode: openh264 emits I420, libyuv
// scales I420, and x265 consumes I420. Keeping one owned, contiguous buffer per
// plane avoids per-frame reallocation churn in the hot path — callers reuse the
// same YuvFrame instance and only resize when the geometry actually changes.
struct YuvFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
    int y_stride = 0;
    int u_stride = 0;
    int v_stride = 0;
    std::int64_t pts_90k = 0; // presentation timestamp on the 90 kHz clock

    // Sizes the planes for a width x height I420 image with tightly packed
    // strides (chroma dimensions rounded up). Existing capacity is reused.
    void allocate(std::uint32_t w, std::uint32_t h) {
        width = w;
        height = h;
        y_stride = static_cast<int>(w);
        u_stride = static_cast<int>((w + 1) / 2);
        v_stride = u_stride;
        const std::size_t chroma_h = (h + 1) / 2;
        y.assign(static_cast<std::size_t>(y_stride) * h, 0);
        u.assign(static_cast<std::size_t>(u_stride) * chroma_h, 0);
        v.assign(static_cast<std::size_t>(v_stride) * chroma_h, 0);
    }

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

} // namespace rtmp_server::transcoding::native
