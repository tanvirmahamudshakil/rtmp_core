#include "rtmp_server/transcoding/backend.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

#include "rtmp_server/core/error.hpp"

extern char** environ;

namespace rtmp_server::transcoding {

namespace {

core::Error unavailable(std::string message) {
    return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Configuration,
                       std::move(message));
}

std::string encoder_for(VideoCodec codec, BackendKind backend) {
    if (codec == VideoCodec::Passthrough) return "copy";
    if (codec == VideoCodec::Disabled) return {};
    if (backend == BackendKind::Nvenc) {
        if (codec == VideoCodec::H264) return "h264_nvenc";
        if (codec == VideoCodec::H265) return "hevc_nvenc";
    }
    if (backend == BackendKind::QuickSync) {
        if (codec == VideoCodec::H264) return "h264_qsv";
        if (codec == VideoCodec::H265) return "hevc_qsv";
        if (codec == VideoCodec::Vp9) return "vp9_qsv";
    }
    if (backend == BackendKind::Software) {
        switch (codec) {
            case VideoCodec::H263: return "h263";
            case VideoCodec::H264: return "libx264";
            case VideoCodec::H265: return "libx265";
            case VideoCodec::Vp8: return "libvpx";
            case VideoCodec::Vp9: return "libvpx-vp9";
            default: break;
        }
    }
    return {};
}

} // namespace

BackendRegistry::BackendRegistry(std::string ffmpeg_path) : ffmpeg_path_(std::move(ffmpeg_path)) {}

bool BackendRegistry::encoder_is_available(std::string_view name) const {
    std::string help = "encoder=" + std::string(name);
    std::vector<std::string> storage{ffmpeg_path_, "-hide_banner", "-loglevel", "error", "-h", std::move(help)};
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) return false;
    ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int spawn_result =
        ::posix_spawn(&pid, ffmpeg_path_.c_str(), &actions, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    if (spawn_result != 0) return false;

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::vector<BackendCapabilities> BackendRegistry::probe() const {
    std::vector<BackendCapabilities> result;
    const bool software = encoder_is_available("libx264") && encoder_is_available("aac");
    result.push_back({BackendKind::Software, software,
                      software ? "libx264 and AAC through the libav media pipeline"
                               : "required software encoders are missing"});
    const bool nvenc = encoder_is_available("h264_nvenc");
    result.push_back({BackendKind::Nvenc, nvenc,
                      nvenc ? "NVIDIA NVENC encoder exposed by libav"
                            : "h264_nvenc is not available in this FFmpeg build"});
    const bool qsv = encoder_is_available("h264_qsv");
    result.push_back({BackendKind::QuickSync, qsv,
                      qsv ? "Intel oneVPL/QuickSync encoder exposed by libav"
                          : "h264_qsv is not available in this FFmpeg build"});
    result.push_back({BackendKind::Beamr, false, "Beamr SDK adapter requires a licensed SDK"});
    result.push_back({BackendKind::MainConcept, false, "MainConcept SDK adapter requires a licensed SDK"});
    return result;
}

core::Result<std::string> BackendRegistry::video_encoder(const Preset& preset) const {
    if (preset.backend == BackendKind::Beamr || preset.backend == BackendKind::MainConcept) {
        return unavailable(std::string(to_string(preset.backend)) + " requires its licensed SDK adapter");
    }
    auto name = encoder_for(preset.video_codec, preset.backend);
    if (name.empty() && preset.video_codec != VideoCodec::Disabled) {
        return unavailable(std::string(to_string(preset.backend)) + " does not support " +
                           std::string(to_string(preset.video_codec)));
    }
    if (!name.empty() && name != "copy" && !encoder_is_available(name)) {
        return unavailable("encoder '" + name + "' is not available");
    }
    return name;
}

core::Result<std::string> BackendRegistry::audio_encoder(const Preset& preset) const {
    std::string encoder;
    switch (preset.audio_codec) {
        case AudioCodec::Aac: encoder = "aac"; break;
        case AudioCodec::Vorbis: encoder = "libvorbis"; break;
        case AudioCodec::Opus: encoder = "libopus"; break;
        case AudioCodec::Passthrough: return std::string("copy");
        case AudioCodec::Disabled: return std::string{};
    }
    if (!encoder_is_available(encoder)) return unavailable("encoder '" + encoder + "' is not available");
    return encoder;
}

} // namespace rtmp_server::transcoding
