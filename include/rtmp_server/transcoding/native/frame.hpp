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
    // strides (chroma dimensions rounded up). Existing storage is reused and
    // deliberately left untouched when the geometry is unchanged: every
    // decoder/scaler caller overwrites the complete destination, so clearing
    // a multi-megabyte 1080p/4K frame before writing it again only burns
    // memory bandwidth on the live hot path.
    void allocate(std::uint32_t w, std::uint32_t h) {
        const int next_y_stride = static_cast<int>(w);
        const int next_uv_stride = static_cast<int>((w + 1) / 2);
        const std::size_t chroma_h = (h + 1) / 2;
        const std::size_t y_size = static_cast<std::size_t>(next_y_stride) * h;
        const std::size_t uv_size = static_cast<std::size_t>(next_uv_stride) * chroma_h;

        width = w;
        height = h;
        y_stride = next_y_stride;
        u_stride = next_uv_stride;
        v_stride = u_stride;
        y.resize(y_size);
        u.resize(uv_size);
        v.resize(uv_size);
    }

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

} // namespace rtmp_server::transcoding::native
