#include "rtmp_server/transcoding/native/geometry.hpp"

#include <algorithm>

namespace rtmp_server::transcoding::native {

namespace {

// Rounds down to an even value; codecs in 4:2:0 need even luma dimensions.
std::uint32_t make_even(std::uint32_t value) noexcept { return value - (value % 2); }

// Clamps a dimension to a sane, even, non-zero value.
std::uint32_t clamp_dim(std::uint32_t value) noexcept {
    if (value < 2) return 2;
    return make_even(value);
}

} // namespace

ScalePlan compute_scale_plan(const Preset& preset, std::uint32_t src_w, std::uint32_t src_h) {
    ScalePlan plan;
    src_w = std::max<std::uint32_t>(src_w, 2);
    src_h = std::max<std::uint32_t>(src_h, 2);

    const bool have_w = preset.width.has_value() && *preset.width > 0;
    const bool have_h = preset.height.has_value() && *preset.height > 0;
    const std::uint32_t tw = have_w ? clamp_dim(*preset.width) : 0;
    const std::uint32_t th = have_h ? clamp_dim(*preset.height) : 0;

    // No target geometry, or explicit MatchSource: encode at the source size.
    if (preset.fit_mode == FitMode::MatchSource || (!have_w && !have_h)) {
        plan.scale_w = plan.crop_w = plan.out_w = clamp_dim(src_w);
        plan.scale_h = plan.crop_h = plan.out_h = clamp_dim(src_h);
        return plan;
    }

    switch (preset.fit_mode) {
        case FitMode::MatchSource:
            break; // handled above
        case FitMode::FitWidth: {
            // Lock width, derive height from source aspect ratio.
            const std::uint32_t w = have_w ? tw : clamp_dim(src_w);
            const std::uint32_t h = clamp_dim(static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(w) * src_h / std::max<std::uint32_t>(src_w, 1)));
            plan.scale_w = plan.crop_w = plan.out_w = w;
            plan.scale_h = plan.crop_h = plan.out_h = h;
            break;
        }
        case FitMode::FitHeight: {
            const std::uint32_t h = have_h ? th : clamp_dim(src_h);
            const std::uint32_t w = clamp_dim(static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(h) * src_w / std::max<std::uint32_t>(src_h, 1)));
            plan.scale_w = plan.crop_w = plan.out_w = w;
            plan.scale_h = plan.crop_h = plan.out_h = h;
            break;
        }
        case FitMode::Stretch: {
            const std::uint32_t w = have_w ? tw : clamp_dim(src_w);
            const std::uint32_t h = have_h ? th : clamp_dim(src_h);
            plan.scale_w = plan.crop_w = plan.out_w = w;
            plan.scale_h = plan.crop_h = plan.out_h = h;
            break;
        }
        case FitMode::Crop: {
            // Scale so the target rectangle is fully covered (aspect-fill),
            // then crop the centre. Requires both target dimensions.
            const std::uint32_t w = have_w ? tw : clamp_dim(src_w);
            const std::uint32_t h = have_h ? th : clamp_dim(src_h);
            const double sx = static_cast<double>(w) / src_w;
            const double sy = static_cast<double>(h) / src_h;
            const double scale = std::max(sx, sy);
            plan.scale_w = clamp_dim(static_cast<std::uint32_t>(src_w * scale + 0.5));
            plan.scale_h = clamp_dim(static_cast<std::uint32_t>(src_h * scale + 0.5));
            plan.crop_w = std::min(w, plan.scale_w);
            plan.crop_h = std::min(h, plan.scale_h);
            plan.crop_x = make_even((plan.scale_w - plan.crop_w) / 2);
            plan.crop_y = make_even((plan.scale_h - plan.crop_h) / 2);
            plan.out_w = plan.crop_w;
            plan.out_h = plan.crop_h;
            break;
        }
        case FitMode::Letterbox: {
            // Scale to fit inside the target rectangle (aspect-fit), then pad
            // the shorter axis with black. Requires both target dimensions.
            const std::uint32_t w = have_w ? tw : clamp_dim(src_w);
            const std::uint32_t h = have_h ? th : clamp_dim(src_h);
            const double sx = static_cast<double>(w) / src_w;
            const double sy = static_cast<double>(h) / src_h;
            const double scale = std::min(sx, sy);
            plan.scale_w = clamp_dim(static_cast<std::uint32_t>(src_w * scale + 0.5));
            plan.scale_h = clamp_dim(static_cast<std::uint32_t>(src_h * scale + 0.5));
            plan.scale_w = std::min(plan.scale_w, w);
            plan.scale_h = std::min(plan.scale_h, h);
            plan.crop_w = plan.scale_w;
            plan.crop_h = plan.scale_h;
            plan.out_w = w;
            plan.out_h = h;
            plan.pad_x = make_even((w - plan.scale_w) / 2);
            plan.pad_y = make_even((h - plan.scale_h) / 2);
            break;
        }
    }

    if (plan.out_w < 2) plan.out_w = 2;
    if (plan.out_h < 2) plan.out_h = 2;
    if (plan.crop_w == 0) plan.crop_w = plan.out_w;
    if (plan.crop_h == 0) plan.crop_h = plan.out_h;
    return plan;
}

} // namespace rtmp_server::transcoding::native
