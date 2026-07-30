#pragma once

#include <cstdint>

#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding::native {

// A fully resolved scaling plan derived from a preset's FitMode and the source
// picture size. The pipeline executes it in three libyuv-friendly steps:
//   1. scale the source to (scale_w x scale_h),
//   2. crop a (out_w x out_h) window at (crop_x, crop_y),
//   3. centre the cropped image inside an (out_w x out_h) canvas at
//      (pad_x, pad_y), letterboxing the remainder.
// For the common fit modes most of these collapse to no-ops; the encoder only
// ever sees an out_w x out_h picture. All dimensions are even (H.264/HEVC 4:2:0
// requires even luma dimensions) and non-zero.
struct ScalePlan {
    std::uint32_t scale_w = 0;
    std::uint32_t scale_h = 0;
    std::uint32_t crop_x = 0;
    std::uint32_t crop_y = 0;
    std::uint32_t crop_w = 0;
    std::uint32_t crop_h = 0;
    std::uint32_t pad_x = 0;
    std::uint32_t pad_y = 0;
    std::uint32_t out_w = 0;
    std::uint32_t out_h = 0;

    [[nodiscard]] bool is_passthrough_size(std::uint32_t src_w, std::uint32_t src_h) const noexcept {
        return scale_w == src_w && scale_h == src_h && crop_w == src_w && crop_h == src_h &&
               pad_x == 0 && pad_y == 0 && out_w == src_w && out_h == src_h;
    }
};

// Computes the scaling plan for a source of (src_w x src_h) under `preset`.
// Pure and deterministic — no allocation, no libraries — so the geometry can be
// unit-tested independently of openh264/libyuv/x265. Returns a plan whose
// out_w/out_h match the source when FitMode::MatchSource is selected or when a
// target dimension is absent.
[[nodiscard]] ScalePlan compute_scale_plan(const Preset& preset, std::uint32_t src_w,
                                           std::uint32_t src_h);

} // namespace rtmp_server::transcoding::native
