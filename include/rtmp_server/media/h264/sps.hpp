#pragma once

#include <cstddef>
#include <span>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/h264/avc.hpp"
#include "rtmp_server/media/video_dimensions.hpp"

// H.264 sequence-parameter-set parsing, limited to the picture geometry.
//
// Needed because every container other than MPEG-TS has to state the picture
// size up front: fMP4's `tkhd`/`avc1` sample entry, a DASH `AdaptationSet`
// `@width/@height`, and the HLS master playlist's RESOLUTION attribute. TS
// never needed it (a TS decoder reads the in-band SPS), which is why nothing
// in this repository parsed an SPS before CMAF packaging existed.
namespace rtmp_server::media::h264 {

// Parses one SPS NAL unit (start code already stripped, NAL header byte
// included) and returns its cropped display size. Fails on a truncated or
// non-SPS NAL rather than returning a guess.
[[nodiscard]] core::Result<VideoDimensions> parse_sps_dimensions(std::span<const std::byte> sps_nal);

// Convenience: the dimensions of the first SPS in a decoder config.
[[nodiscard]] core::Result<VideoDimensions> parse_dimensions(const AvcDecoderConfig& config);

} // namespace rtmp_server::media::h264
