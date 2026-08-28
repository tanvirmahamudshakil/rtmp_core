#include "rtmp_server/transcoding/native/source_transcoder.hpp"

#include <algorithm>
#include <optional>

#include "rtmp_server/transcoding/native/geometry.hpp"

namespace rtmp_server::transcoding::native {
namespace {

ScalePlan resolved_plan(const RenditionSpec& spec, std::uint32_t source_width,
                        std::uint32_t source_height) {
    Preset preset;
    if (spec.width) preset.width = spec.width;
    if (spec.height) preset.height = spec.height;
    preset.fit_mode = spec.fit_mode;
    return compute_scale_plan(preset, source_width, source_height);
}

} // namespace

std::vector<std::uint32_t> allocate_rendition_video_threads(
    std::span<const RenditionSpec> renditions, std::uint32_t source_width,
    std::uint32_t source_height, std::uint32_t core_budget) {
    std::vector<std::uint32_t> threads(renditions.size(), 1);
    if (renditions.empty()) return threads;

    const std::size_t budget = std::max<std::size_t>(core_budget, 1);
    if (renditions.size() >= budget) return threads;

    std::vector<std::uint64_t> pixels;
    std::vector<std::uint32_t> desired;
    pixels.reserve(renditions.size());
    desired.reserve(renditions.size());
    for (const auto& spec : renditions) {
        const auto plan = resolved_plan(spec, source_width, source_height);
        const auto area = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(plan.out_w) * plan.out_h);
        pixels.push_back(area);
        if (area >= 3840ULL * 2160ULL) {
            desired.push_back(8);
        } else if (area >= 1920ULL * 1080ULL) {
            desired.push_back(4);
        } else if (area >= 1280ULL * 720ULL) {
            desired.push_back(2);
        } else {
            desired.push_back(1);
        }
    }

    std::size_t remaining = budget - renditions.size();
    // First approach each resolution tier's useful threading floor without
    // starving equally sized renditions. Maximising desired/current is a
    // small integer water-fill and, unlike independent per-rendition minima,
    // can never oversubscribe the job's assigned cores.
    while (remaining > 0) {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < threads.size(); ++i) {
            if (threads[i] >= desired[i]) continue;
            if (!best || static_cast<std::uint64_t>(desired[i]) * threads[*best] >
                             static_cast<std::uint64_t>(desired[*best]) * threads[i]) {
                best = i;
            }
        }
        if (!best) break;
        ++threads[*best];
        --remaining;
    }

    // On a large box, distribute spare slices in proportion to output area.
    // x264 itself stops benefiting once a frame cannot be sliced further;
    // the encoder wrapper also enforces the same 16-thread ceiling.
    while (remaining > 0) {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < threads.size(); ++i) {
            if (threads[i] >= 16) continue;
            if (!best || pixels[i] * (threads[*best] + 1ULL) >
                             pixels[*best] * (threads[i] + 1ULL)) {
                best = i;
            }
        }
        if (!best) break;
        ++threads[*best];
        --remaining;
    }
    return threads;
}

} // namespace rtmp_server::transcoding::native
