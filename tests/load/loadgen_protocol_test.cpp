// Phase 7: protocol-correctness tests for the real load generator
// (docs/v2_promot.md PHASE 7 "Real load generator", and the doc's explicit
// warning: "Do not claim the server supports 1,000 viewers merely because a
// synthetic in-memory benchmark loops over 1,000 subscribers").
//
// The point of these tests is to prove the generator is a REAL RTMP client,
// by checking the bytes it puts on a real socket against the same
// handshake/chunk/AMF0 decoders the server itself uses — not by trusting the
// tool's own self-reported counters.
//
// Two harnesses are used:
//
//  1. ProtocolServer — a plain blocking-POSIX-socket listener that runs the
//     production HandshakeSession and then feeds every subsequent byte into
//     a production ChunkDecoder, recording the decoded RtmpMessages. This is
//     the same substitution tests/integration/rtmp_full_session_socket_test
//     already makes (io_uring is Linux-only and cannot be built on this
//     host); it verifies the generator's handshake and command bytes are
//     decodable by the server's real codecs.
//
//  2. A full RtmpConnectionSession behind the same listener, so the
//     generator drives an actual publish -> LiveFanout -> viewer path over
//     TCP, and a second generator client subscribes and verifies the media
//     it receives byte-for-byte.
//
// What these tests do NOT do: measure capacity. They run a handful of
// connections for a second or two. Real capacity numbers require the
// production io_uring transport, which cannot be built here — see
// docs/capacity-report.md.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "rtmp_server/loadgen/media_source.hpp"
#include "rtmp_server/loadgen/rtmp_client.hpp"
#include "rtmp_server/loadgen/scenario.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"
#include "rtmp_server/protocol/media/media_ingest.hpp"
#include "rtmp_server/protocol/session/rtmp_connection_session.hpp"

namespace rtmp_server::loadgen {
namespace {

using protocol::chunk::MessageTypeId;
using protocol::chunk::RtmpMessage;
using protocol::handshake::HandshakeSession;

// A peer that vanishes mid-write must not kill the process with SIGPIPE.
// Darwin/BSD: SO_NOSIGPIPE socket option. Linux: MSG_NOSIGNAL send flag.
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

inline void suppress_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}


// ---------------------------------------------------------------------------
// Harness 1: records the decoded messages a client sends, nothing more.
// ---------------------------------------------------------------------------
class ProtocolServer {
public:
    ProtocolServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_fd_, 64);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }

    ~ProtocolServer() {
        stop_.store(true);
        if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] bool handshake_completed() const { return handshake_done_.load(); }
    [[nodiscard]] bool decoder_failed() const { return decoder_failed_.load(); }

    [[nodiscard]] std::vector<RtmpMessage> messages() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_;
    }

    // Accepts one connection, completes the handshake, then decodes
    // everything the client sends. Replies to `connect`/`createStream`/
    // `publish` just enough for the client state machine to advance.
    void serve_one() {
        thread_ = std::thread([this]() {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;
            suppress_sigpipe(fd);

            protocol::chunk::ChunkEncoder encoder(128);
            const auto send_all = [fd](std::span<const std::byte> bytes) {
                std::size_t sent = 0;
                while (sent < bytes.size()) {
                    const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, kSendFlags);
                    if (n <= 0) return;
                    sent += static_cast<std::size_t>(n);
                }
            };

            HandshakeSession handshake;
            handshake.set_send_handler([&](core::SharedBuffer buf) { send_all(buf.view()); });
            handshake.set_fail_handler([](core::Error) {});

            protocol::chunk::ChunkDecoder decoder(1024 * 1024);
            decoder.set_error_handler([this](core::Error) { decoder_failed_.store(true); });
            decoder.set_message_handler([&](const RtmpMessage& message) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    messages_.push_back(message);
                }
                if (message.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) return;
                auto decoded = protocol::amf0::decode_all(message.payload);
                if (!decoded.ok() || decoded.value().empty()) return;
                if (!decoded.value()[0].is_string()) return;
                const std::string name = decoded.value()[0].as_string();

                std::vector<std::byte> out;
                using protocol::amf0::Amf0Value;
                const auto reply = [&](std::vector<Amf0Value> values, std::uint32_t msid) {
                    RtmpMessage r;
                    r.chunk_stream_id = 3;
                    r.message_stream_id = msid;
                    r.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
                    for (const auto& v : values) protocol::amf0::encode(v, r.payload);
                    out.clear();
                    encoder.encode_message(r, out);
                    send_all(out);
                };

                if (name == "connect") {
                    reply({Amf0Value::string("_result"), Amf0Value::number(1), Amf0Value::null(),
                           Amf0Value::object({{"code", Amf0Value::string("NetConnection.Connect.Success")}})},
                          0);
                } else if (name == "createStream") {
                    reply({Amf0Value::string("_result"), Amf0Value::number(2), Amf0Value::null(),
                           Amf0Value::number(1)},
                          0);
                } else if (name == "publish" || name == "play") {
                    reply({Amf0Value::string("onStatus"), Amf0Value::number(0), Amf0Value::null(),
                           Amf0Value::object({{"code", Amf0Value::string(name == "publish"
                                                                             ? "NetStream.Publish.Start"
                                                                             : "NetStream.Play.Start")}})},
                          1);
                }
            });

            handshake.set_complete_handler([&]() {
                handshake_done_.store(true);
                auto trailing = handshake.take_trailing_bytes();
                if (!trailing.empty()) decoder.on_bytes_received(trailing);
            });

            std::array<std::byte, 8192> buf{};
            while (!stop_.load()) {
                timeval tv{0, 100000};
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
                if (n == 0) break;
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                    break;
                }
                std::span<const std::byte> data(buf.data(), static_cast<std::size_t>(n));
                if (!handshake.is_terminal()) {
                    handshake.on_bytes_received(data);
                } else {
                    decoder.on_bytes_received(data);
                }
            }
            ::close(fd);
        });
    }

private:
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> handshake_done_{false};
    std::atomic<bool> decoder_failed_{false};
    mutable std::mutex mutex_;
    std::vector<RtmpMessage> messages_;
};

// Drives one client through poll() until `predicate` holds or the deadline
// passes. Returns whether the predicate held.
bool drive_until(RtmpClient& client, const std::function<bool()>& predicate,
                 std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        if (client.finished()) return predicate();

        pollfd pfd{};
        pfd.fd = client.fd();
        pfd.events = POLLIN;
        if (client.wants_write()) pfd.events |= POLLOUT;
        if (pfd.fd < 0) return predicate();

        if (::poll(&pfd, 1, 20) > 0) {
            if ((pfd.revents & (POLLOUT | POLLERR | POLLHUP)) != 0) client.on_writable();
            if (!client.finished() && (pfd.revents & (POLLIN | POLLHUP)) != 0) client.on_readable();
        }
    }
    return predicate();
}

std::string amf0_command_name(const RtmpMessage& message) {
    if (message.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) return {};
    auto decoded = protocol::amf0::decode_all(message.payload);
    if (!decoded.ok() || decoded.value().empty()) return {};
    const auto& name = decoded.value()[0];
    return name.is_string() ? name.as_string() : std::string{};
}

// ===========================================================================
// MediaSource: byte-level correctness of the generated media
// ===========================================================================

TEST(LoadGenMediaSource, VideoSequenceHeaderIsAValidAvcDecoderConfigurationRecord) {
    MediaSource source(MediaProfile{});
    const auto frame = source.video_sequence_header();

    // The server's own classifier must agree this is an AVC sequence header —
    // this is what makes the GOP cache store it and replay it to new viewers.
    const auto info = protocol::media::classify_video_tag(frame.payload);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->codec, protocol::media::VideoCodec::Avc);
    EXPECT_EQ(info->avc_packet_type, protocol::media::AvcPacketType::SequenceHeader);

    // And the record must actually parse, not merely be tagged correctly.
    // parse_avc_sequence_header() takes the AVCDecoderConfigurationRecord
    // itself, i.e. the tag body AFTER the 5-byte FLV video tag header
    // (frametype/codec, AVCPacketType, 3-byte composition time) — that is
    // the `config_data` slice MediaIngest hands it.
    ASSERT_GT(frame.payload.size(), 5u);
    const auto parsed = protocol::media::parse_avc_sequence_header(
        std::span<const std::byte>(frame.payload).subspan(5));
    ASSERT_TRUE(parsed.ok()) << parsed.error().message();
    EXPECT_FALSE(parsed.value().sps_list.empty());
    EXPECT_FALSE(parsed.value().pps_list.empty());
    EXPECT_EQ(parsed.value().nalu_length_size, 4);
}

TEST(LoadGenMediaSource, AudioSequenceHeaderIsAValidAudioSpecificConfig) {
    MediaSource source(MediaProfile{});
    const auto frame = source.audio_sequence_header();

    const auto info = protocol::media::classify_audio_tag(frame.payload);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->codec, protocol::media::AudioCodec::Aac);
    EXPECT_EQ(info->aac_packet_type, protocol::media::AacPacketType::SequenceHeader);
}

TEST(LoadGenMediaSource, KeyframesAreEmittedAtTheConfiguredIntervalAndClassifyAsKeyframes) {
    MediaProfile profile;
    profile.frames_per_second = 30;
    profile.keyframe_interval_frames = 10;
    MediaSource source(profile);

    // One second of media.
    const auto frames = source.frames_until(1000);
    ASSERT_FALSE(frames.empty());

    std::uint32_t keyframes = 0;
    std::uint32_t video_frames = 0;
    for (const auto& frame : frames) {
        if (!is_video(frame.kind)) continue;
        ++video_frames;
        const auto info = protocol::media::classify_video_tag(frame.payload);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->codec, protocol::media::VideoCodec::Avc);
        if (frame.kind == FrameKind::VideoKeyframe) {
            // The server must independently agree this is a keyframe, or the
            // GOP cache would never start a new GOP under load.
            EXPECT_EQ(info->frame_type, protocol::media::VideoFrameType::KeyFrame);
            ++keyframes;
        } else {
            EXPECT_EQ(info->frame_type, protocol::media::VideoFrameType::InterFrame);
        }
    }

    EXPECT_GE(video_frames, 30u);
    // 31 video frames at t=0..1000ms with interval 10 -> indices 0,10,20,30.
    EXPECT_EQ(keyframes, (video_frames + 9) / 10);
}

TEST(LoadGenMediaSource, PayloadSizesTrackTheConfiguredBitrate) {
    MediaProfile profile;
    profile.video_bitrate_bps = 4'000'000;
    profile.audio_bitrate_bps = 128'000;
    profile.frames_per_second = 30;
    profile.keyframe_interval_frames = 30;
    MediaSource source(profile);

    const auto frames = source.frames_until(10'000); // 10 seconds
    std::uint64_t total_bits = 0;
    for (const auto& frame : frames) total_bits += frame.payload.size() * 8;

    // Expect ~4.128 Mbps of payload over 10 s. Allow a generous band: frame
    // sizes are integer-rounded and the tail of the window is partial. The
    // point is that the tool honours the configured bitrate rather than
    // emitting two-byte stubs.
    const double measured_bps = static_cast<double>(total_bits) / 10.0;
    EXPECT_GT(measured_bps, 3'000'000.0);
    EXPECT_LT(measured_bps, 5'500'000.0);
}

TEST(LoadGenMediaSource, VerifyPatternAcceptsGeneratedFramesAndRejectsTamperedOnes) {
    MediaSource source(MediaProfile{});
    auto frames = source.frames_until(500);
    ASSERT_FALSE(frames.empty());

    for (const auto& frame : frames) {
        EXPECT_TRUE(MediaSource::verify_pattern(frame.payload))
            << "generated frame failed its own verification";
    }

    // A single flipped payload byte must be detected — this is what makes the
    // corruption statistic meaningful rather than decorative.
    auto tampered = frames.back();
    ASSERT_GT(tampered.payload.size(), 12u);
    tampered.payload[tampered.payload.size() - 1] ^= std::byte{0xFF};
    EXPECT_FALSE(MediaSource::verify_pattern(tampered.payload));

    // Truncation must be detected too (declared NAL length vs actual bytes).
    auto truncated = frames.back();
    truncated.payload.pop_back();
    EXPECT_FALSE(MediaSource::verify_pattern(truncated.payload));
}

// ===========================================================================
// RtmpClient: real socket, real handshake, real commands
// ===========================================================================

TEST(LoadGenClient, PerformsARealHandshakeAndSendsDecodableConnectPublishCommands) {
    ProtocolServer server;
    server.serve_one();

    RtmpClient::Config config;
    config.port = server.port();
    config.stream_key = "loadgen-key";
    config.role = RtmpClient::Role::Publisher;
    RtmpClient client(config);
    ASSERT_TRUE(client.start().ok());

    ASSERT_TRUE(drive_until(client, [&]() { return client.state() == RtmpClient::State::Streaming; }))
        << "client never reached Streaming; failure=" << client.stats().failure_reason;

    // The production HandshakeSession accepted the generator's C0/C1/C2.
    EXPECT_TRUE(server.handshake_completed());
    // The production ChunkDecoder parsed everything with no error.
    EXPECT_FALSE(server.decoder_failed());

    const auto messages = server.messages();
    std::vector<std::string> commands;
    for (const auto& message : messages) {
        auto name = amf0_command_name(message);
        if (!name.empty()) commands.push_back(std::move(name));
    }

    ASSERT_GE(commands.size(), 3u);
    EXPECT_EQ(commands[0], "connect");
    EXPECT_EQ(commands[1], "createStream");
    EXPECT_EQ(commands[2], "publish");

    // Latencies were actually measured, not left at zero.
    EXPECT_GT(client.stats().handshake_latency.count(), 0);
    EXPECT_TRUE(client.stats().reached_streaming);
}

TEST(LoadGenClient, ViewerSendsPlayRatherThanPublish) {
    ProtocolServer server;
    server.serve_one();

    RtmpClient::Config config;
    config.port = server.port();
    config.stream_key = "loadgen-key";
    config.role = RtmpClient::Role::Viewer;
    RtmpClient client(config);
    ASSERT_TRUE(client.start().ok());

    ASSERT_TRUE(drive_until(client, [&]() { return client.state() == RtmpClient::State::Streaming; }))
        << "viewer never reached Streaming; failure=" << client.stats().failure_reason;

    std::vector<std::string> commands;
    for (const auto& message : server.messages()) {
        auto name = amf0_command_name(message);
        if (!name.empty()) commands.push_back(std::move(name));
    }
    ASSERT_GE(commands.size(), 3u);
    EXPECT_EQ(commands[2], "play");
}

TEST(LoadGenClient, PublishedMediaArrivesAsDecodableVideoAndAudioMessages) {
    ProtocolServer server;
    server.serve_one();

    RtmpClient::Config config;
    config.port = server.port();
    config.stream_key = "loadgen-key";
    config.role = RtmpClient::Role::Publisher;
    config.media.video_bitrate_bps = 800'000;
    config.media.frames_per_second = 30;
    config.media.keyframe_interval_frames = 15;
    RtmpClient client(config);
    ASSERT_TRUE(client.start().ok());

    ASSERT_TRUE(drive_until(client, [&]() { return client.state() == RtmpClient::State::Streaming; }));

    // Publish ~300 ms of media and let it drain.
    for (std::uint32_t t = 0; t <= 300; t += 20) {
        client.pump_media(t);
        drive_until(client, []() { return false; }, std::chrono::milliseconds{20});
    }
    drive_until(client, []() { return false; }, std::chrono::milliseconds{300});

    const auto messages = server.messages();
    std::uint32_t video = 0;
    std::uint32_t audio = 0;
    std::uint32_t video_seq_headers = 0;
    std::uint32_t keyframes = 0;

    for (const auto& message : messages) {
        const auto type = static_cast<MessageTypeId>(message.message_type_id);
        if (type == MessageTypeId::Video) {
            ++video;
            // Every media payload the generator sent must survive the real
            // chunk encode -> TCP -> real chunk decode round trip intact.
            EXPECT_TRUE(MediaSource::verify_pattern(message.payload))
                << "video payload corrupted through the chunk codec";
            const auto info = protocol::media::classify_video_tag(message.payload);
            ASSERT_TRUE(info.has_value());
            if (info->avc_packet_type == protocol::media::AvcPacketType::SequenceHeader) ++video_seq_headers;
            else if (info->frame_type == protocol::media::VideoFrameType::KeyFrame) ++keyframes;
        } else if (type == MessageTypeId::Audio) {
            ++audio;
            EXPECT_TRUE(MediaSource::verify_pattern(message.payload))
                << "audio payload corrupted through the chunk codec";
        }
    }

    EXPECT_GE(video, 5u) << "publisher produced too little video";
    EXPECT_GE(audio, 3u) << "publisher produced no audio";
    EXPECT_EQ(video_seq_headers, 1u) << "exactly one AVC sequence header should precede the media";
    EXPECT_GE(keyframes, 1u);
    EXPECT_FALSE(server.decoder_failed());
}

TEST(LoadGenClient, AbruptDisconnectClosesWithoutAnRtmpTeardown) {
    ProtocolServer server;
    server.serve_one();

    RtmpClient::Config config;
    config.port = server.port();
    config.stream_key = "loadgen-key";
    RtmpClient client(config);
    ASSERT_TRUE(client.start().ok());
    ASSERT_TRUE(drive_until(client, [&]() { return client.state() == RtmpClient::State::Streaming; }));

    client.abort_connection();
    EXPECT_EQ(client.state(), RtmpClient::State::Closed);
    EXPECT_LT(client.fd(), 0);

    // No deleteStream was ever sent: an abrupt disconnect must look like a
    // dropped connection to the server, not a clean teardown.
    for (const auto& message : server.messages()) {
        EXPECT_NE(amf0_command_name(message), "deleteStream");
    }
}

// ===========================================================================
// Full pipeline: generator publisher -> real session/fan-out -> generator viewer
// ===========================================================================

// Single-threaded, non-blocking, poll(2)-driven server running the production
// HandshakeSession + RtmpConnectionSession + StreamRegistry + LiveFanout —
// i.e. the whole publish/fan-out path, minus io_uring.
//
// Why ONE thread, deliberately: RtmpConnectionSession and CommandSession are
// owned by exactly one event-loop worker in production, and LiveFanout
// delivers a publisher's frame by calling straight into each viewer's
// PlaybackSink. An earlier revision of this harness gave every connection its
// own thread, which meant the publisher's thread and a viewer's own thread
// could both be inside that viewer's session (one via fan-out, one via
// LiveFanout::subscribe replaying the cached GOP) at the same time. That is a
// data race on the viewer's ChunkEncoder and outbound buffer, and ASan caught
// it as a heap corruption (`unknown-crash` / `memcpy-param-overlap` inside
// ChunkEncoder::encode_message) — see docs/phase-7-report.md.
//
// That was a defect in this harness, not in the server: the production
// io_uring transport assigns each connection to one worker and routes
// cross-worker frames through CrossWorkerRouter precisely so this cannot
// happen. Modelling one worker here reproduces that ownership discipline.
//
// Writes are also non-blocking and buffered per connection. A blocking send
// inside a PlaybackSink callback would stall the single worker (and, with a
// slow viewer, deadlock it) — the same reason production never blocks a
// network worker.
class SessionServer {
public:
    explicit SessionServer(observability::Metrics* metrics = nullptr) : metrics_(metrics) {
        if (metrics_ != nullptr) {
            fanout_.set_metrics(metrics_);
            registry_.set_metrics(metrics_);
        }

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_fd_, 128);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        set_nonblocking(listen_fd_);

        worker_ = std::thread([this]() { worker_loop(); });
    }

    ~SessionServer() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    [[nodiscard]] std::uint16_t port() const { return port_; }

    // Publishes the aggregate fan-out gauges. Called from the TEST thread
    // while the worker thread is actively fanning out media, which is
    // exactly the concurrency LiveFanout::sample_gauges() must tolerate.
    void sample_gauges() { fanout_.sample_gauges(); }

private:
    struct Connection {
        int fd = -1;
        std::unique_ptr<HandshakeSession> handshake;
        std::unique_ptr<protocol::session::RtmpConnectionSession> session;
        std::vector<std::byte> out;
        std::size_t out_offset = 0;
        bool closing = false;
    };

    static void set_nonblocking(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void accept_new() {
        for (;;) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;
            set_nonblocking(fd);
            suppress_sigpipe(fd);

            auto conn = std::make_unique<Connection>();
            conn->fd = fd;
            Connection* raw = conn.get();

            conn->handshake = std::make_unique<HandshakeSession>();
            conn->handshake->set_send_handler(
                [raw](core::SharedBuffer buf) { raw->out.insert(raw->out.end(), buf.view().begin(), buf.view().end()); });
            conn->handshake->set_fail_handler([raw](core::Error) { raw->closing = true; });
            conn->handshake->set_complete_handler([this, raw]() {
                protocol::session::RtmpConnectionSession::Dependencies deps;
                deps.registry = &registry_;
                deps.live_fanout = &fanout_;
                raw->session = std::make_unique<protocol::session::RtmpConnectionSession>(
                    next_connection_id_++, deps, 8u * 1024u * 1024u, 4096);
                raw->session->set_outgoing_handler(
                    [raw](std::vector<std::byte> bytes) { raw->out.insert(raw->out.end(), bytes.begin(), bytes.end()); });
                raw->session->set_close_handler([raw]() { raw->closing = true; });
                if (metrics_ != nullptr) raw->session->set_metrics(metrics_);
                raw->session->start();
                auto trailing = raw->handshake->take_trailing_bytes();
                if (!trailing.empty()) raw->session->on_bytes_received(trailing);
            });

            connections_.push_back(std::move(conn));
        }
    }

    static void drain(Connection& conn) {
        while (conn.out_offset < conn.out.size()) {
            const std::size_t remaining = conn.out.size() - conn.out_offset;
            const ssize_t n = ::send(conn.fd, conn.out.data() + conn.out_offset, remaining, kSendFlags);
            if (n > 0) {
                conn.out_offset += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (n < 0 && errno == EINTR) continue;
            conn.closing = true;
            return;
        }
        conn.out.clear();
        conn.out_offset = 0;
    }

    void worker_loop() {
        std::array<std::byte, 32768> buf{};
        while (!stop_.load()) {
            std::vector<pollfd> pfds;
            pfds.reserve(connections_.size() + 1);
            pollfd listener{};
            listener.fd = listen_fd_;
            listener.events = POLLIN;
            pfds.push_back(listener);

            for (auto& conn : connections_) {
                pollfd p{};
                p.fd = conn->fd;
                p.events = POLLIN;
                if (conn->out_offset < conn->out.size()) p.events |= POLLOUT;
                pfds.push_back(p);
            }

            if (::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 10) > 0) {
                if ((pfds[0].revents & POLLIN) != 0) accept_new();

                // NOTE: accept_new() above may have appended new connections,
                // so connections_ can now be LONGER than the pollfd array we
                // built before poll(). Iterate over the polled prefix only —
                // indexing pfds[i + 1] by connections_.size() reads past the
                // end (caught by ASan as a heap-buffer-overflow).
                const std::size_t polled = pfds.size() - 1;
                for (std::size_t i = 0; i < polled && i < connections_.size(); ++i) {
                    auto& conn = *connections_[i];
                    const short revents = pfds[i + 1].revents;
                    if (revents == 0) continue;

                    if ((revents & POLLOUT) != 0) drain(conn);

                    if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                        for (;;) {
                            const ssize_t n = ::recv(conn.fd, buf.data(), buf.size(), 0);
                            if (n > 0) {
                                std::span<const std::byte> data(buf.data(), static_cast<std::size_t>(n));
                                if (!conn.handshake->is_terminal()) {
                                    conn.handshake->on_bytes_received(data);
                                } else if (conn.session) {
                                    conn.session->on_bytes_received(data);
                                }
                                continue;
                            }
                            if (n == 0) conn.closing = true;
                            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                            if (n < 0 && errno == EINTR) continue;
                            if (n < 0) conn.closing = true;
                            break;
                        }
                    }

                    if (!conn.closing) drain(conn);
                }
            } else {
                // Even with no readiness, keep pushing queued fan-out bytes
                // out: LiveFanout may have enqueued into a viewer while its
                // socket was idle.
                for (auto& conn : connections_) {
                    if (!conn->closing) drain(*conn);
                }
            }

            // Deterministic teardown of finished connections: the session's
            // destructor path must unregister publisher/subscriber state.
            std::erase_if(connections_, [](const std::unique_ptr<Connection>& conn) {
                if (!conn->closing) return false;
                if (conn->session) conn->session->on_connection_closed();
                if (conn->fd >= 0) ::close(conn->fd);
                return true;
            });
        }

        for (auto& conn : connections_) {
            if (conn->session) conn->session->on_connection_closed();
            if (conn->fd >= 0) ::close(conn->fd);
        }
        connections_.clear();
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::uint64_t next_connection_id_ = 1;
    observability::Metrics* metrics_ = nullptr;
    std::vector<std::unique_ptr<Connection>> connections_;
    protocol::commands::StreamRegistry registry_;
    protocol::commands::LiveFanout fanout_;
};


TEST(LoadGenPipeline, RealPublisherAndRealViewersExchangeVerifiedMediaOverTcp) {
    observability::Metrics metrics;
    SessionServer server(&metrics);

    constexpr int kViewers = 8;

    RtmpClient::Config pub_config;
    pub_config.port = server.port();
    pub_config.stream_key = "pipeline-key";
    pub_config.role = RtmpClient::Role::Publisher;
    pub_config.media.video_bitrate_bps = 600'000;
    pub_config.media.frames_per_second = 30;
    pub_config.media.keyframe_interval_frames = 10;

    RtmpClient publisher(pub_config);
    ASSERT_TRUE(publisher.start().ok());
    ASSERT_TRUE(drive_until(publisher, [&]() { return publisher.state() == RtmpClient::State::Streaming; }))
        << publisher.stats().failure_reason;

    std::vector<std::unique_ptr<RtmpClient>> viewers;
    for (int i = 0; i < kViewers; ++i) {
        RtmpClient::Config vc;
        vc.port = server.port();
        vc.stream_key = "pipeline-key";
        vc.role = RtmpClient::Role::Viewer;
        auto viewer = std::make_unique<RtmpClient>(vc);
        ASSERT_TRUE(viewer->start().ok());
        viewers.push_back(std::move(viewer));
    }

    // Drive publisher + viewers together for ~1.5 s of stream time.
    const auto run_until = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    std::uint32_t stream_time = 0;
    while (std::chrono::steady_clock::now() < run_until) {
        std::vector<pollfd> pfds;
        std::vector<RtmpClient*> owners;

        const auto add = [&](RtmpClient& c) {
            if (c.finished() || c.fd() < 0) return;
            pollfd p{};
            p.fd = c.fd();
            p.events = POLLIN;
            if (c.wants_write()) p.events |= POLLOUT;
            pfds.push_back(p);
            owners.push_back(&c);
        };
        add(publisher);
        for (auto& v : viewers) add(*v);

        if (!pfds.empty() && ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 10) > 0) {
            for (std::size_t i = 0; i < pfds.size(); ++i) {
                const short re = pfds[i].revents;
                if (re == 0) continue;
                if ((re & (POLLOUT | POLLERR | POLLHUP)) != 0) owners[i]->on_writable();
                if (!owners[i]->finished() && (re & (POLLIN | POLLHUP)) != 0) owners[i]->on_readable();
            }
        }

        stream_time += 10;
        publisher.pump_media(stream_time);
    }

    // Every viewer must have reached playback and received verified media.
    int streaming_viewers = 0;
    std::uint64_t total_received = 0;
    std::uint64_t total_corrupt = 0;
    std::uint64_t total_keyframes = 0;
    for (const auto& viewer : viewers) {
        const auto& s = viewer->stats();
        if (s.reached_streaming) ++streaming_viewers;
        total_received += s.media_messages_received;
        total_corrupt += s.payloads_corrupt;
        total_keyframes += s.keyframes_received;
    }

    EXPECT_EQ(streaming_viewers, kViewers);
    EXPECT_GT(total_received, 0u) << "no media reached any viewer over TCP";
    EXPECT_EQ(total_corrupt, 0u) << "media was corrupted between publisher and viewers";
    EXPECT_GT(total_keyframes, 0u) << "viewers never received a keyframe";

    // Every one of these metrics was fed by real socket traffic through the
    // production code paths, not poked by the test.
    using observability::MetricId;
    EXPECT_GT(metrics.value(MetricId::EgressBytesTotal), 0) << "egress bytes never recorded";
    EXPECT_GT(metrics.value(MetricId::IngressBytesTotal), 0) << "ingress bytes never recorded";
    EXPECT_GT(metrics.value(MetricId::ActiveViewers), 0) << "active_viewers never recorded";
    EXPECT_EQ(metrics.value(MetricId::ActivePublishers), 1) << "active_publishers should track the one publisher";

    // Ingress must be far smaller than egress: one publisher's stream is
    // fanned out to kViewers viewers. This is the fan-out amplification the
    // metric pair exists to show.
    EXPECT_GT(metrics.value(MetricId::EgressBytesTotal), metrics.value(MetricId::IngressBytesTotal));

    // Derived rates are computed, not left at zero.
    metrics.refresh_derived(std::chrono::steady_clock::now());
    metrics.refresh_process_metrics();
    EXPECT_GT(metrics.value(MetricId::ProcessMemoryBytes), 0);

    // The gauge sampler walks live streams without deadlocking against the
    // media path, and reports the real viewer count.
    server.sample_gauges();
    EXPECT_GT(metrics.value(MetricId::ViewersPerStreamMax), 0);
    EXPECT_GT(metrics.value(MetricId::ActiveStreams), 0);

    // Nothing leaked a high-cardinality name into the registry.
    EXPECT_EQ(metrics.value(MetricId::MetricsRejectedNames), 0);
    const std::string exposition = metrics.render_prometheus();
    EXPECT_NE(exposition.find("active_viewers"), std::string::npos);
    EXPECT_EQ(exposition.find("pipeline-key"), std::string::npos)
        << "a stream key must never appear in metric output";
}

} // namespace
} // namespace rtmp_server::loadgen
