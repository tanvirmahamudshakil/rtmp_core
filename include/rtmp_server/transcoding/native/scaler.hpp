#pragma once

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/geometry.hpp"

namespace rtmp_server::transcoding::native {

// Executes a ScalePlan on an I420 frame using libyuv: box-filtered scale, then
// centre crop and/or letterbox pad, producing the exact out_w x out_h picture
// the encoder expects. A reusable scratch buffer avoids per-frame allocation.
class Scaler {
public:
    // Transforms `src` into `dst` according to `plan`. When the plan is a size
    // passthrough this is a plane copy. `dst` is (re)allocated to the plan's
    // output geometry and inherits `src`'s timestamp.
    [[nodiscard]] core::Result<void> scale(const YuvFrame& src, const ScalePlan& plan, YuvFrame& dst);

private:
    YuvFrame scratch_; // holds the scaled-but-not-yet-cropped/padded image
};

} // namespace rtmp_server::transcoding::native
