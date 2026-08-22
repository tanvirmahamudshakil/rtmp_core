#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/backend.hpp"
#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding {

// Shared ffmpeg-argv building blocks used both by TranscoderSupervisor (whose
// input is a locally-published RTMP stream) and by the Source Transcode job
// runner (whose input is an arbitrary external rtmp://, http:// or https://
// URL). The output side — one rendition's -map/-c:v/-c:a/bitrate/filter
// flags, terminated by a `-f flv rtmp://loopback/...` push — is identical in
// both cases, so it lives here once instead of being duplicated.

[[nodiscard]] std::string ffmpeg_even_dimension(std::uint32_t value);
[[nodiscard]] std::optional<std::string> ffmpeg_video_filter(const Preset& preset);
void ffmpeg_append_arg(std::vector<std::string>& args, std::string key, std::string value);

// Appends the full output-side argument sequence for one rendition (video
// map/codec/filter/bitrate flags, audio map/codec/bitrate flags, and the
// trailing `-f flv <destination_url>`) to `args`. `concurrent_encoders` caps
// libx264 thread usage so multiple simultaneous software encodes do not each
// try to claim every CPU core.
[[nodiscard]] core::Result<void> ffmpeg_append_rendition_output(
    std::vector<std::string>& args, const BackendRegistry& backends, const Preset& preset,
    std::size_t concurrent_encoders, const std::string& destination_url);

} // namespace rtmp_server::transcoding
