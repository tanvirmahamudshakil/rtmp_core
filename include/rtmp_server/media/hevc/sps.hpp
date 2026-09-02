#pragma once

#include <cstddef>
#include <span>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/hevc/hevc.hpp"
#include "rtmp_server/media/video_dimensions.hpp"

// HEVC sequence-parameter-set geometry parsing — the H.265 counterpart of
// media::h264::parse_sps_dimensions, needed by the same fMP4/DASH/master
// playlist callers.
namespace rtmp_server::media::hevc {

// Parses one SPS NAL unit (start code stripped, 2-byte NAL header included)
// and returns its conformance-window-cropped display size.
[[nodiscard]] core::Result<VideoDimensions> parse_sps_dimensions(std::span<const std::byte> sps_nal);

[[nodiscard]] core::Result<VideoDimensions> parse_dimensions(const HevcDecoderConfig& config);

} // namespace rtmp_server::media::hevc
