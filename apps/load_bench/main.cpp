#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "rtmp_server/core/clock.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/commands/command_session.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"

// load_bench [--streams N] [--viewers-per-stream M] [--frames F]
//
// In-process synthetic load test for the RTMP protocol/fan-out layer
// (docs/rtmp_promot.md Phase 9 "load testing"). There is no real socket
// transport on this host (io_uring is Linux-only — see every phase 4-9
// checklist's "Known limitations"), so this drives the exact same
// CommandSession/StreamRegistry/LiveFanout objects the real transport would,
// with in-memory RtmpMessage handoffs instead of socket I/O — it measures
// the protocol/fan-out layer's own throughput ceiling, not network I/O,
// which is the part this codebase actually owns end-to-end on any host.
//
// For each of `--streams` simulated streams: one publisher CommandSession
// publishes, `--viewers-per-stream` viewer CommandSessions play it, then the
// publisher pushes `--frames` synthetic video frames through the full
// publish -> route_media_message -> LiveFanout -> viewer relay ->
// outgoing_handler_ path. Reports elapsed time and frames/sec fanned out.

namespace {

using rtmp_server::core::MonotonicClock;
using rtmp_server::protocol::amf0::Amf0Value;
using rtmp_server::protocol::chunk::MessageTypeId;
using rtmp_server::protocol::chunk::RtmpMessage;
using rtmp_server::protocol::commands::CommandSession;
using rtmp_server::protocol::commands::LiveFanout;
using rtmp_server::protocol::commands::StreamKeyValidator;
using rtmp_server::protocol::commands::StreamRegistry;

RtmpMessage make_command(std::uint32_t message_stream_id, std::vector<Amf0Value> values) {
    RtmpMessage msg;
    msg.chunk_stream_id = 3;
    msg.message_stream_id = message_stream_id;
    msg.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    for (const auto& v : values) rtmp_server::protocol::amf0::encode(v, msg.payload);
    return msg;
}

struct Options {
    int streams = 4;
    int viewers_per_stream = 50;
    int frames = 1000;
};

Options parse_options(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        auto next_int = [&](int fallback) {
            if (i + 1 >= argc) return fallback;
            return std::atoi(argv[++i]);
        };
        if (std::strcmp(argv[i], "--streams") == 0) opts.streams = next_int(opts.streams);
        else if (std::strcmp(argv[i], "--viewers-per-stream") == 0) opts.viewers_per_stream = next_int(opts.viewers_per_stream);
        else if (std::strcmp(argv[i], "--frames") == 0) opts.frames = next_int(opts.frames);
    }
    return opts;
}

} // namespace

int main(int argc, char** argv) {
    Options opts = parse_options(argc, argv);
    std::printf("load_bench: streams=%d viewers_per_stream=%d frames=%d\n", opts.streams, opts.viewers_per_stream,
                opts.frames);

    StreamRegistry registry;
    LiveFanout fanout;
    StreamKeyValidator always_ok = [](std::string_view, std::string_view) { return true; };

    std::vector<std::unique_ptr<CommandSession>> publishers;
    std::vector<std::unique_ptr<CommandSession>> viewers;
    std::vector<std::uint32_t> publisher_stream_ids;
    std::uint64_t delivered_messages = 0;
    std::uint64_t next_connection_id = 1;

    for (int s = 0; s < opts.streams; ++s) {
        std::string key = "bench-key-" + std::to_string(s);

        auto publisher = std::make_unique<CommandSession>(next_connection_id++, registry, always_ok);
        publisher->set_outgoing_handler([](RtmpMessage) {});
        publisher->set_live_fanout(&fanout);
        publisher->handle_message(make_command(
            0, {Amf0Value::string("connect"), Amf0Value::number(1), Amf0Value::object({{"app", Amf0Value::string("live")}})}));
        publisher->handle_message(make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
        std::uint32_t pub_stream_id = publisher->last_created_stream_id();
        publisher->handle_message(make_command(
            pub_stream_id, {Amf0Value::string("publish"), Amf0Value::number(0), Amf0Value::null(),
                            Amf0Value::string(key), Amf0Value::string("live")}));
        publisher_stream_ids.push_back(pub_stream_id);
        publishers.push_back(std::move(publisher));

        for (int v = 0; v < opts.viewers_per_stream; ++v) {
            auto viewer = std::make_unique<CommandSession>(next_connection_id++, registry, always_ok);
            viewer->set_outgoing_handler([&delivered_messages](RtmpMessage) { ++delivered_messages; });
            viewer->set_live_fanout(&fanout);
            viewer->handle_message(make_command(
                0, {Amf0Value::string("connect"), Amf0Value::number(1), Amf0Value::object({{"app", Amf0Value::string("live")}})}));
            viewer->handle_message(make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
            std::uint32_t view_stream_id = viewer->last_created_stream_id();
            viewer->handle_message(make_command(
                view_stream_id, {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(), Amf0Value::string(key)}));
            viewers.push_back(std::move(viewer));
        }
    }

    delivered_messages = 0; // ignore Play.Start replies already counted above
    auto start = MonotonicClock::now();

    for (int f = 0; f < opts.frames; ++f) {
        bool keyframe = (f % 30) == 0;
        RtmpMessage video;
        video.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Video);
        video.timestamp = static_cast<std::uint32_t>(f * 33);
        video.payload = {static_cast<std::byte>(keyframe ? 0x17 : 0x27), static_cast<std::byte>(0x01)};
        for (std::size_t s = 0; s < publishers.size(); ++s) {
            video.message_stream_id = publisher_stream_ids[s];
            publishers[s]->handle_message(video);
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(MonotonicClock::now() - start);
    double seconds = elapsed.count();
    double fps = seconds > 0 ? static_cast<double>(delivered_messages) / seconds : 0.0;

    std::printf("delivered %llu viewer messages in %.3fs (%.0f messages/sec)\n",
                static_cast<unsigned long long>(delivered_messages), seconds, fps);
    return EXIT_SUCCESS;
}
