#pragma once

#include <cstdint>

namespace rtmp_server::media {

// Coded picture size in luma samples, after the cropping/conformance window
// has been applied — i.e. the display size a container's track header and a
// playlist's RESOLUTION attribute must advertise, not the macroblock-aligned
// coded size.
struct VideoDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
};

} // namespace rtmp_server::media
