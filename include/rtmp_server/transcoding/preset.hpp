#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::transcoding {

enum class BackendKind {
    Software,
    Nvenc,
    QuickSync,
    Beamr,
    MainConcept,
};

enum class VideoCodec {
    H263,
    H264,
    H265,
    Vp8,
    Vp9,
    Passthrough,
    Disabled,
};

enum class AudioCodec {
    Aac,
    Vorbis,
    Opus,
    Passthrough,
    Disabled,
};

enum class FitMode {
    MatchSource,
    FitWidth,
    FitHeight,
    Crop,
    Stretch,
    Letterbox,
};

struct Preset {
    std::string name;
    std::string outgoing_stream_name;
    std::string description;
    BackendKind backend = BackendKind::Software;
    VideoCodec video_codec = VideoCodec::H264;
    std::uint64_t video_bitrate = 2'500'000;
    std::string profile = "high";
    std::optional<std::uint32_t> keyframe_interval;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    FitMode fit_mode = FitMode::MatchSource;
    AudioCodec audio_codec = AudioCodec::Aac;
    std::uint64_t audio_bitrate = 128'000;
    std::optional<std::uint32_t> gpu_id;
};

struct Rule {
    std::string application;
    std::string source_stream;
    std::vector<Preset> presets;
};

class PresetCatalogue {
public:
    [[nodiscard]] static core::Result<PresetCatalogue> load(const std::string& path);
    [[nodiscard]] static core::Result<PresetCatalogue> parse(std::string_view rules);
    [[nodiscard]] std::vector<Preset> match(std::string_view application,
                                             std::string_view source_stream) const;
    [[nodiscard]] const std::vector<Rule>& rules() const noexcept { return rules_; }
    void upsert_rule(Rule rule);
    void remove_rule(std::string_view application, std::string_view source_stream);

private:
    [[nodiscard]] static core::Result<PresetCatalogue> parse_stream(std::istream& input);
    std::vector<Rule> rules_;
};

[[nodiscard]] std::string_view to_string(BackendKind value);
[[nodiscard]] std::string_view to_string(VideoCodec value);
[[nodiscard]] std::string_view to_string(AudioCodec value);
[[nodiscard]] std::string_view to_string(FitMode value);

} // namespace rtmp_server::transcoding
