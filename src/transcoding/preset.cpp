#include "rtmp_server/transcoding/preset.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding {

namespace {

using core::Error;
using core::ErrorCategory;
using core::ErrorCode;
using core::Result;

Error invalid(std::size_t line, std::string message) {
    return Error(ErrorCode::InvalidConfiguration, ErrorCategory::Configuration,
                 "transcoding preset line " + std::to_string(line) + ": " + std::move(message));
}

void trim(std::string& value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) {
        value.clear();
    } else {
        value.assign(first, last);
    }
}

std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        fields.emplace_back(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        trim(fields.back());
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return fields;
}

bool valid_name(std::string_view value, bool wildcard = false) {
    if (value.empty() || value.size() > 128) return false;
    if (wildcard && value == "*") return true;
    return std::ranges::all_of(value, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
    });
}

bool valid_output_name(std::string_view value) {
    std::string expanded(value);
    static constexpr std::string_view token = "{source}";
    std::size_t position = 0;
    while ((position = expanded.find(token, position)) != std::string::npos) {
        expanded.replace(position, token.size(), "source");
        position += 6;
    }
    return valid_name(expanded);
}

template <typename T>
std::optional<T> number(std::string_view text) {
    if (text.empty()) return std::nullopt;
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<BackendKind> backend(std::string_view value) {
    if (value == "default" || value == "software") return BackendKind::Software;
    if (value == "nvenc") return BackendKind::Nvenc;
    if (value == "quicksync" || value == "qsv") return BackendKind::QuickSync;
    if (value == "beamr") return BackendKind::Beamr;
    if (value == "mainconcept") return BackendKind::MainConcept;
    return std::nullopt;
}

std::optional<VideoCodec> video_codec(std::string_view value) {
    if (value == "h263") return VideoCodec::H263;
    if (value == "h264") return VideoCodec::H264;
    if (value == "h265" || value == "hevc") return VideoCodec::H265;
    if (value == "vp8") return VideoCodec::Vp8;
    if (value == "vp9") return VideoCodec::Vp9;
    if (value == "passthrough" || value == "copy") return VideoCodec::Passthrough;
    if (value == "disabled" || value == "none") return VideoCodec::Disabled;
    return std::nullopt;
}

std::optional<AudioCodec> audio_codec(std::string_view value) {
    if (value == "aac") return AudioCodec::Aac;
    if (value == "vorbis") return AudioCodec::Vorbis;
    if (value == "opus") return AudioCodec::Opus;
    if (value == "passthrough" || value == "copy") return AudioCodec::Passthrough;
    if (value == "disabled" || value == "none") return AudioCodec::Disabled;
    return std::nullopt;
}

std::optional<FitMode> fit_mode(std::string_view value) {
    if (value == "match-source") return FitMode::MatchSource;
    if (value == "fit-width") return FitMode::FitWidth;
    if (value == "fit-height") return FitMode::FitHeight;
    if (value == "crop") return FitMode::Crop;
    if (value == "stretch") return FitMode::Stretch;
    if (value == "letterbox") return FitMode::Letterbox;
    return std::nullopt;
}

} // namespace

Result<PresetCatalogue> PresetCatalogue::load(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return Error(ErrorCode::MissingConfiguration, ErrorCategory::Configuration,
                     "transcoding preset file not found: " + path);
    }
    return parse_stream(input);
}

Result<PresetCatalogue> PresetCatalogue::parse(std::string_view rules) {
    std::istringstream input{std::string(rules)};
    return parse_stream(input);
}

Result<PresetCatalogue> PresetCatalogue::parse_stream(std::istream& input) {
    PresetCatalogue catalogue;
    std::unordered_map<std::string, std::size_t> rule_by_selector;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (const auto comment = line.find('#'); comment != std::string::npos) line.erase(comment);
        trim(line);
        if (line.empty()) continue;

        auto fields = split(line, '|');
        if (fields.size() < 14 || fields.size() > 15) {
            return invalid(line_number, "expected 14 pipe-separated fields plus an optional description");
        }

        const auto slash = fields[0].find('/');
        if (slash == std::string::npos || fields[0].find('/', slash + 1) != std::string::npos) {
            return invalid(line_number, "source selector must be application/stream");
        }
        std::string application = fields[0].substr(0, slash);
        std::string source = fields[0].substr(slash + 1);
        if (!valid_name(application) || !valid_name(source, true)) {
            return invalid(line_number, "application/stream contains unsupported characters");
        }
        if (!valid_name(fields[1]) || !valid_output_name(fields[2])) {
            return invalid(line_number, "preset/output names contain unsupported characters");
        }
        std::string resolved_output = fields[2];
        if (source != "*") {
            std::size_t position = 0;
            while ((position = resolved_output.find("{source}", position)) != std::string::npos) {
                resolved_output.replace(position, 8, source);
                position += source.size();
            }
        }
        if ((source == "*" && fields[2] == "{source}") ||
            (source != "*" && source == resolved_output)) {
            return invalid(line_number, "outgoing stream must not equal the source stream");
        }

        auto parsed_backend = backend(fields[3]);
        auto parsed_video = video_codec(fields[4]);
        auto parsed_fit = fit_mode(fields[10]);
        auto parsed_audio = audio_codec(fields[11]);
        if (!parsed_backend) return invalid(line_number, "unknown backend '" + fields[3] + "'");
        if (!parsed_video) return invalid(line_number, "unknown video codec '" + fields[4] + "'");
        if (!parsed_fit) return invalid(line_number, "unknown fit mode '" + fields[10] + "'");
        if (!parsed_audio) return invalid(line_number, "unknown audio codec '" + fields[11] + "'");

        Preset preset;
        preset.name = fields[1];
        preset.outgoing_stream_name = fields[2];
        preset.backend = *parsed_backend;
        preset.video_codec = *parsed_video;
        preset.fit_mode = *parsed_fit;
        preset.audio_codec = *parsed_audio;
        preset.profile = fields[6].empty() ? "high" : fields[6];
        if (preset.profile != "baseline" && preset.profile != "main" && preset.profile != "high") {
            return invalid(line_number, "profile must be baseline, main or high");
        }

        if (preset.video_codec != VideoCodec::Disabled && preset.video_codec != VideoCodec::Passthrough) {
            auto bitrate = number<std::uint64_t>(fields[5]);
            if (!bitrate || *bitrate < 50'000 || *bitrate > 200'000'000) {
                return invalid(line_number, "video bitrate must be 50000..200000000 bps");
            }
            preset.video_bitrate = *bitrate;
        }
        if (!fields[7].empty() && fields[7] != "source") {
            auto interval = number<std::uint32_t>(fields[7]);
            if (!interval || *interval < 1 || *interval > 1000) {
                return invalid(line_number, "keyframe interval must be source or 1..1000 frames");
            }
            preset.keyframe_interval = *interval;
        }
        if (!fields[8].empty()) {
            preset.width = number<std::uint32_t>(fields[8]);
            if (!preset.width) return invalid(line_number, "frame width must be an integer");
        }
        if (!fields[9].empty()) {
            preset.height = number<std::uint32_t>(fields[9]);
            if (!preset.height) return invalid(line_number, "frame height must be an integer");
        }
        if ((preset.width && (*preset.width < 16 || *preset.width > 8192)) ||
            (preset.height && (*preset.height < 16 || *preset.height > 8192))) {
            return invalid(line_number, "frame dimensions must be 16..8192");
        }
        if ((preset.width && *preset.width % 2 != 0) ||
            (preset.height && *preset.height % 2 != 0)) {
            return invalid(line_number, "frame dimensions must be even numbers");
        }
        if (preset.fit_mode == FitMode::Stretch || preset.fit_mode == FitMode::Crop ||
            preset.fit_mode == FitMode::Letterbox) {
            if (!preset.width || !preset.height) {
                return invalid(line_number, "stretch, crop and letterbox require width and height");
            }
        }
        if (preset.fit_mode == FitMode::FitWidth && !preset.width) {
            return invalid(line_number, "fit-width requires width");
        }
        if (preset.fit_mode == FitMode::FitHeight && !preset.height) {
            return invalid(line_number, "fit-height requires height");
        }
        if (preset.audio_codec != AudioCodec::Disabled && preset.audio_codec != AudioCodec::Passthrough) {
            auto bitrate = number<std::uint64_t>(fields[12]);
            if (!bitrate || *bitrate < 16'000 || *bitrate > 1'536'000) {
                return invalid(line_number, "audio bitrate must be 16000..1536000 bps");
            }
            preset.audio_bitrate = *bitrate;
        }
        if (!fields[13].empty() && fields[13] != "first") {
            auto gpu = number<std::uint32_t>(fields[13]);
            if (!gpu || *gpu > 63) return invalid(line_number, "GPU ID must be first or 0..63");
            preset.gpu_id = *gpu;
        }
        if (fields.size() == 15) preset.description = fields[14];

        const std::string selector = application + "/" + source;
        auto [it, inserted] = rule_by_selector.emplace(selector, catalogue.rules_.size());
        if (inserted) catalogue.rules_.push_back(Rule{std::move(application), std::move(source), {}});
        auto& presets = catalogue.rules_[it->second].presets;
        if (std::ranges::any_of(presets, [&preset](const Preset& existing) {
                return existing.outgoing_stream_name == preset.outgoing_stream_name;
            })) {
            return invalid(line_number, "duplicate outgoing stream within one source rule");
        }
        presets.push_back(std::move(preset));
    }

    return catalogue;
}

std::vector<Preset> PresetCatalogue::match(std::string_view application,
                                            std::string_view source_stream) const {
    auto expand = [source_stream](std::vector<Preset> presets) {
        for (auto& preset : presets) {
            std::size_t position = 0;
            while ((position = preset.outgoing_stream_name.find("{source}", position)) != std::string::npos) {
                preset.outgoing_stream_name.replace(position, 8, source_stream);
                position += source_stream.size();
            }
        }
        return presets;
    };
    for (const auto& rule : rules_) {
        if (rule.application == application && rule.source_stream == source_stream) return expand(rule.presets);
    }
    for (const auto& rule : rules_) {
        if (rule.application == application && rule.source_stream == "*") return expand(rule.presets);
    }
    return {};
}

void PresetCatalogue::upsert_rule(Rule rule) {
    auto it = std::ranges::find_if(rules_, [&rule](const Rule& existing) {
        return existing.application == rule.application &&
               existing.source_stream == rule.source_stream;
    });
    if (it == rules_.end()) rules_.push_back(std::move(rule));
    else *it = std::move(rule);
}

void PresetCatalogue::remove_rule(std::string_view application, std::string_view source_stream) {
    std::erase_if(rules_, [application, source_stream](const Rule& rule) {
        return rule.application == application && rule.source_stream == source_stream;
    });
}

std::string_view to_string(BackendKind value) {
    switch (value) {
        case BackendKind::Software: return "software";
        case BackendKind::Nvenc: return "nvenc";
        case BackendKind::QuickSync: return "quicksync";
        case BackendKind::Beamr: return "beamr";
        case BackendKind::MainConcept: return "mainconcept";
    }
    return "unknown";
}

std::string_view to_string(VideoCodec value) {
    switch (value) {
        case VideoCodec::H263: return "h263";
        case VideoCodec::H264: return "h264";
        case VideoCodec::H265: return "h265";
        case VideoCodec::Vp8: return "vp8";
        case VideoCodec::Vp9: return "vp9";
        case VideoCodec::Passthrough: return "passthrough";
        case VideoCodec::Disabled: return "disabled";
    }
    return "unknown";
}

std::string_view to_string(AudioCodec value) {
    switch (value) {
        case AudioCodec::Aac: return "aac";
        case AudioCodec::Vorbis: return "vorbis";
        case AudioCodec::Opus: return "opus";
        case AudioCodec::Passthrough: return "passthrough";
        case AudioCodec::Disabled: return "disabled";
    }
    return "unknown";
}

std::string_view to_string(FitMode value) {
    switch (value) {
        case FitMode::MatchSource: return "match-source";
        case FitMode::FitWidth: return "fit-width";
        case FitMode::FitHeight: return "fit-height";
        case FitMode::Crop: return "crop";
        case FitMode::Stretch: return "stretch";
        case FitMode::Letterbox: return "letterbox";
    }
    return "unknown";
}

} // namespace rtmp_server::transcoding
