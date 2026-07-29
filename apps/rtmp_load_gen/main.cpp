// rtmp_load_gen — Phase 7 real RTMP load generator
// (docs/v2_promot.md PHASE 7 "Real load generator").
//
// Unlike apps/load_bench, which calls CommandSession::handle_message()
// in-process and therefore measures only the protocol layer's own ceiling,
// every client this tool creates opens a real TCP socket, performs a real
// RTMP handshake, sends real chunk-encoded AMF0 commands, and publishes or
// receives real FLV-shaped H.264/AAC payloads. Numbers it reports are
// network numbers.
//
// Usage:
//   rtmp_load_gen [options]
//
//   --host H                    server address           (default 127.0.0.1)
//   --port P                    server port              (default 1935)
//   --app A                     RTMP application         (default live)
//   --key-prefix S              stream key prefix        (default loadtest-)
//   --publish-key S             exact secret key, single-publisher production mode
//   --playback-name S           exact public name paired with --publish-key
//   --publishers N              publisher count          (default 1)
//   --viewers N                 viewers per publisher    (default 100)
//   --duration S                run length in seconds    (default 30)
//   --ramp-up MS                connection ramp window   (default 5000)
//   --video-bitrate BPS         per-publisher video rate (default 2500000)
//   --audio-bitrate BPS         per-publisher audio rate (default 128000)
//   --fps N                     video frame rate         (default 30)
//   --keyframe-interval N       keyframe every N frames  (default 60)
//   --slow-viewers F            fraction 0..1 of slow readers      (default 0)
//   --slow-read-budget B        bytes/tick for slow viewers        (default 4096)
//   --abrupt-disconnects F      fraction 0..1 that RST at halfway  (default 0)
//   --publisher-reconnect S     reconnect every S seconds, 0=never (default 0)
//   --tick MS                   poll/media granularity   (default 20)
//
// Exit status is 0 only when every requested publisher and viewer reaches
// streaming with zero corrupt payloads and zero client failures.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "rtmp_server/loadgen/scenario.hpp"
#include "rtmp_server/observability/logger.hpp"

namespace {

using rtmp_server::loadgen::ScenarioConfig;

struct ArgParser {
    int argc;
    char** argv;
    int index = 1;

    [[nodiscard]] bool matches(const char* name) const { return std::strcmp(argv[index], name) == 0; }

    [[nodiscard]] const char* next_value(const char* fallback) {
        if (index + 1 >= argc) return fallback;
        return argv[++index];
    }
};

} // namespace

int main(int argc, char** argv) {
    ScenarioConfig config;

    ArgParser parser{argc, argv};
    for (; parser.index < argc; ++parser.index) {
        if (parser.matches("--host")) {
            config.host = parser.next_value(config.host.c_str());
        } else if (parser.matches("--port")) {
            config.port = static_cast<std::uint16_t>(std::atoi(parser.next_value("1935")));
        } else if (parser.matches("--app")) {
            config.application = parser.next_value(config.application.c_str());
        } else if (parser.matches("--key-prefix")) {
            config.stream_key_prefix = parser.next_value(config.stream_key_prefix.c_str());
        } else if (parser.matches("--publish-key")) {
            config.publish_key = parser.next_value("");
        } else if (parser.matches("--playback-name")) {
            config.playback_name = parser.next_value("");
        } else if (parser.matches("--publishers")) {
            config.publishers = static_cast<std::uint32_t>(std::atoi(parser.next_value("1")));
        } else if (parser.matches("--viewers")) {
            config.viewers_per_publisher = static_cast<std::uint32_t>(std::atoi(parser.next_value("100")));
        } else if (parser.matches("--duration")) {
            config.duration = std::chrono::seconds{std::atoi(parser.next_value("30"))};
        } else if (parser.matches("--ramp-up")) {
            config.ramp_up = std::chrono::milliseconds{std::atoi(parser.next_value("5000"))};
        } else if (parser.matches("--video-bitrate")) {
            config.media.video_bitrate_bps = static_cast<std::uint32_t>(std::atoi(parser.next_value("2500000")));
        } else if (parser.matches("--audio-bitrate")) {
            config.media.audio_bitrate_bps = static_cast<std::uint32_t>(std::atoi(parser.next_value("128000")));
        } else if (parser.matches("--fps")) {
            config.media.frames_per_second = static_cast<std::uint32_t>(std::atoi(parser.next_value("30")));
        } else if (parser.matches("--keyframe-interval")) {
            config.media.keyframe_interval_frames = static_cast<std::uint32_t>(std::atoi(parser.next_value("60")));
        } else if (parser.matches("--slow-viewers")) {
            config.slow_viewer_fraction = std::strtod(parser.next_value("0"), nullptr);
        } else if (parser.matches("--slow-read-budget")) {
            config.slow_viewer_read_budget = static_cast<std::size_t>(std::atoi(parser.next_value("4096")));
        } else if (parser.matches("--abrupt-disconnects")) {
            config.abrupt_disconnect_fraction = std::strtod(parser.next_value("0"), nullptr);
        } else if (parser.matches("--publisher-reconnect")) {
            config.publisher_reconnect_interval = std::chrono::seconds{std::atoi(parser.next_value("0"))};
        } else if (parser.matches("--tick")) {
            config.tick = std::chrono::milliseconds{std::atoi(parser.next_value("20"))};
        } else {
            std::fprintf(stderr, "unknown option: %s\n", argv[parser.index]);
            return EXIT_FAILURE;
        }
    }

    std::printf("rtmp_load_gen -> %s:%u app=%s key-prefix=%s\n", config.host.c_str(), config.port,
                config.application.c_str(),
                // Stream keys are publish secrets in this server's model, so
                // even the operator-supplied prefix is printed redacted —
                // the same rule the server's own logs follow.
                rtmp_server::observability::redact(config.stream_key_prefix).c_str());
    std::printf("publishers=%u viewers/publisher=%u duration=%llds ramp-up=%lldms\n", config.publishers,
                config.viewers_per_publisher, static_cast<long long>(config.duration.count()),
                static_cast<long long>(config.ramp_up.count()));
    std::printf("video=%u bps @ %u fps, keyframe every %u frames; audio=%u bps\n", config.media.video_bitrate_bps,
                config.media.frames_per_second, config.media.keyframe_interval_frames,
                config.media.audio_bitrate_bps);
    std::fflush(stdout);

    const auto report = rtmp_server::loadgen::run_scenario(config);
    std::fputs(report.to_text().c_str(), stdout);

    const bool healthy =
        report.payloads_corrupt == 0 && report.clients_failed == 0 &&
        report.publishers_streaming == report.publishers_requested &&
        report.viewers_streaming == report.viewers_requested;
    if (!healthy) {
        std::fprintf(stderr,
                     "FAIL: corrupt=%llu failed=%u publishers=%u/%u viewers=%u/%u\n",
                     static_cast<unsigned long long>(report.payloads_corrupt),
                     report.clients_failed, report.publishers_streaming,
                     report.publishers_requested, report.viewers_streaming,
                     report.viewers_requested);
    }
    return healthy ? EXIT_SUCCESS : EXIT_FAILURE;
}
