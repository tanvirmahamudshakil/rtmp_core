#include "rtmp_server/transcoding/native/scaler.hpp"

#include <libyuv.h>

#include <cstring>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error scale_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

// Fills a freshly allocated I420 frame with limited-range black (Y=16, C=128).
void fill_black(YuvFrame& frame) {
    std::memset(frame.y.data(), 16, frame.y.size());
    std::memset(frame.u.data(), 128, frame.u.size());
    std::memset(frame.v.data(), 128, frame.v.size());
}

// Copies a (w x h) luma-aligned region from src(sx,sy) to dst(dx,dy). Chroma
// coordinates/sizes are halved; all inputs are even so this stays integral.
void blit(YuvFrame& dst, std::uint32_t dx, std::uint32_t dy, const YuvFrame& src, std::uint32_t sx,
          std::uint32_t sy, std::uint32_t w, std::uint32_t h) {
    const auto dy_pitch = static_cast<std::size_t>(dst.y_stride);
    const auto sy_pitch = static_cast<std::size_t>(src.y_stride);
    for (std::uint32_t r = 0; r < h; ++r) {
        std::memcpy(dst.y.data() + (dy + r) * dy_pitch + dx,
                    src.y.data() + (sy + r) * sy_pitch + sx, w);
    }
    const std::uint32_t cw = w / 2, ch = h / 2;
    const std::uint32_t cdx = dx / 2, cdy = dy / 2, csx = sx / 2, csy = sy / 2;
    const auto du_pitch = static_cast<std::size_t>(dst.u_stride);
    const auto dv_pitch = static_cast<std::size_t>(dst.v_stride);
    const auto su_pitch = static_cast<std::size_t>(src.u_stride);
    const auto sv_pitch = static_cast<std::size_t>(src.v_stride);
    for (std::uint32_t r = 0; r < ch; ++r) {
        std::memcpy(dst.u.data() + (cdy + r) * du_pitch + cdx,
                    src.u.data() + (csy + r) * su_pitch + csx, cw);
        std::memcpy(dst.v.data() + (cdy + r) * dv_pitch + cdx,
                    src.v.data() + (csy + r) * sv_pitch + csx, cw);
    }
}

} // namespace

core::Result<void> Scaler::scale(const YuvFrame& src, const ScalePlan& plan, YuvFrame& dst) {
    if (src.empty()) return scale_error("scaler received an empty source frame");

    // Fast path: geometry unchanged — straight copy.
    if (plan.is_passthrough_size(src.width, src.height)) {
        dst.allocate(src.width, src.height);
        dst.pts_90k = src.pts_90k;
        blit(dst, 0, 0, src, 0, 0, src.width, src.height);
        return {};
    }

    // Step 1: box-filtered scale into the scratch buffer.
    scratch_.allocate(plan.scale_w, plan.scale_h);
    const int rc = libyuv::I420Scale(
        src.y.data(), src.y_stride, src.u.data(), src.u_stride, src.v.data(), src.v_stride,
        static_cast<int>(src.width), static_cast<int>(src.height), scratch_.y.data(),
        scratch_.y_stride, scratch_.u.data(), scratch_.u_stride, scratch_.v.data(),
        scratch_.v_stride, static_cast<int>(plan.scale_w), static_cast<int>(plan.scale_h),
        libyuv::kFilterBox);
    if (rc != 0) return scale_error("libyuv I420Scale failed");

    // Step 2: compose crop + pad into the output canvas.
    dst.allocate(plan.out_w, plan.out_h);
    dst.pts_90k = src.pts_90k;
    if (plan.pad_x != 0 || plan.pad_y != 0 || plan.out_w != plan.crop_w ||
        plan.out_h != plan.crop_h) {
        fill_black(dst);
    }
    const std::uint32_t cw = std::min(plan.crop_w, plan.scale_w);
    const std::uint32_t ch = std::min(plan.crop_h, plan.scale_h);
    blit(dst, plan.pad_x, plan.pad_y, scratch_, plan.crop_x, plan.crop_y, cw, ch);
    return {};
}

} // namespace rtmp_server::transcoding::native
