#include "rtmp_server/transcoding/ffmpeg_args.hpp"

#include <algorithm>
#include <thread>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding {

namespace {

core::Error config_error(std::string message) {
    return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Configuration,
                       std::move(message));
}

} // namespace

std::string ffmpeg_even_dimension(std::uint32_t value) { return std::to_string(value - (value % 2)); }

void ffmpeg_append_arg(std::vector<std::string>& args, std::string key, std::string value) {
    args.push_back(std::move(key));
    args.push_back(std::move(value));
}

std::optional<std::string> ffmpeg_video_filter(const Preset& preset) {
    switch (preset.fit_mode) {
        case FitMode::MatchSource:
            return std::nullopt;
        case FitMode::FitWidth:
            return "scale=" + ffmpeg_even_dimension(*preset.width) + ":-2";
        case FitMode::FitHeight:
            return "scale=-2:" + ffmpeg_even_dimension(*preset.height);
        case FitMode::Stretch:
            return "scale=" + ffmpeg_even_dimension(*preset.width) + ":" +
                   ffmpeg_even_dimension(*preset.height);
        case FitMode::Crop: {
            const auto width = ffmpeg_even_dimension(*preset.width);
            const auto height = ffmpeg_even_dimension(*preset.height);
            return "scale=" + width + ":" + height +
                   ":force_original_aspect_ratio=increase,crop=" + width + ":" + height;
        }
        case FitMode::Letterbox: {
            const auto width = ffmpeg_even_dimension(*preset.width);
            const auto height = ffmpeg_even_dimension(*preset.height);
            return "scale=" + width + ":" + height +
                   ":force_original_aspect_ratio=decrease,pad=" + width + ":" + height +
                   ":(ow-iw)/2:(oh-ih)/2:black";
        }
    }
    return std::nullopt;
}

core::Result<void> ffmpeg_append_rendition_output(std::vector<std::string>& args,
                                                    const BackendRegistry& backends,
                                                    const Preset& preset,
                                                    std::size_t concurrent_encoders,
                                                    const std::string& destination_url) {
    if (preset.video_codec == VideoCodec::Passthrough && ffmpeg_video_filter(preset)) {
        return config_error("passthrough video cannot change frame size or fit mode");
    }

    auto video_encoder = backends.video_encoder(preset);
    if (!video_encoder) return video_encoder.error();
    auto audio_encoder = backends.audio_encoder(preset);
    if (!audio_encoder) return audio_encoder.error();

    if (preset.video_codec == VideoCodec::Disabled) {
        args.push_back("-vn");
    } else {
        ffmpeg_append_arg(args, "-map", "0:v:0?");
        ffmpeg_append_arg(args, "-c:v", video_encoder.value());
        if (preset.video_codec != VideoCodec::Passthrough) {
            if (auto filter = ffmpeg_video_filter(preset)) ffmpeg_append_arg(args, "-vf", *filter);
            ffmpeg_append_arg(args, "-b:v", std::to_string(preset.video_bitrate));
            ffmpeg_append_arg(args, "-maxrate", std::to_string(preset.video_bitrate));
            ffmpeg_append_arg(args, "-bufsize", std::to_string(preset.video_bitrate * 2));
            ffmpeg_append_arg(args, "-profile:v", preset.profile);
            ffmpeg_append_arg(args, "-pix_fmt", "yuv420p");
            if (preset.keyframe_interval) {
                ffmpeg_append_arg(args, "-g", std::to_string(*preset.keyframe_interval));
                ffmpeg_append_arg(args, "-keyint_min", std::to_string(*preset.keyframe_interval));
            } else {
                ffmpeg_append_arg(args, "-force_key_frames", "source");
            }
            ffmpeg_append_arg(args, "-sc_threshold", "0");
            if (preset.backend == BackendKind::Software) {
                ffmpeg_append_arg(args, "-preset", "veryfast");
                ffmpeg_append_arg(args, "-tune", "zerolatency");
                // libx264 defaults to one thread per CPU core. Left uncapped,
                // concurrent encoders each try to claim every core and
                // contend with each other, turning a real-time encode into a
                // behind-realtime one. Divide the host's cores across the
                // worst-case concurrent encoder count.
                const auto hardware = std::thread::hardware_concurrency();
                const std::size_t divisor = std::max<std::size_t>(1, concurrent_encoders);
                const std::size_t threads =
                    std::max<std::size_t>(1, hardware > 0 ? hardware / divisor : 1);
                ffmpeg_append_arg(args, "-threads", std::to_string(threads));
            } else if (preset.backend == BackendKind::Nvenc) {
                ffmpeg_append_arg(args, "-preset", "p4");
                ffmpeg_append_arg(args, "-tune", "ll");
                if (preset.gpu_id) ffmpeg_append_arg(args, "-gpu", std::to_string(*preset.gpu_id));
            } else if (preset.backend == BackendKind::QuickSync) {
                ffmpeg_append_arg(args, "-preset", "medium");
            }
        }
    }

    if (preset.audio_codec == AudioCodec::Disabled) {
        args.push_back("-an");
    } else {
        ffmpeg_append_arg(args, "-map", "0:a:0?");
        ffmpeg_append_arg(args, "-c:a", audio_encoder.value());
        if (preset.audio_codec != AudioCodec::Passthrough) {
            ffmpeg_append_arg(args, "-b:a", std::to_string(preset.audio_bitrate));
            ffmpeg_append_arg(args, "-af", "aresample=async=1:first_pts=0");
        }
    }
    ffmpeg_append_arg(args, "-flvflags", "no_duration_filesize");
    ffmpeg_append_arg(args, "-f", "flv");
    args.push_back(destination_url);
    return {};
}

} // namespace rtmp_server::transcoding
