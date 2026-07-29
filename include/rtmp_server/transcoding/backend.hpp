#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding {

struct BackendCapabilities {
    BackendKind kind = BackendKind::Software;
    bool available = false;
    std::string detail;
};

// Maps the stable StreamForge preset model onto encoder implementations.
// The process boundary is deliberate: a codec crash or vendor-driver fault
// must not terminate the io_uring origin.
class BackendRegistry {
public:
    explicit BackendRegistry(std::string ffmpeg_path = "/usr/bin/ffmpeg");

    [[nodiscard]] const std::string& ffmpeg_path() const noexcept { return ffmpeg_path_; }
    [[nodiscard]] std::vector<BackendCapabilities> probe() const;
    [[nodiscard]] core::Result<std::string> video_encoder(const Preset& preset) const;
    [[nodiscard]] core::Result<std::string> audio_encoder(const Preset& preset) const;

private:
    [[nodiscard]] bool encoder_is_available(std::string_view name) const;

    std::string ffmpeg_path_;
};

} // namespace rtmp_server::transcoding
