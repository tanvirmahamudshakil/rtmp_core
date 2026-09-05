#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"
#include "rtmp_server/relay/rtmp_push_client.hpp"

using namespace rtmp_server;
using rtmp_server::media::HandoffMessage;
using rtmp_server::media::MediaHandoffQueue;
using rtmp_server::protocol::amf0::Amf0Value;
using rtmp_server::protocol::chunk::MessageTypeId;
using rtmp_server::protocol::chunk::RtmpMessage;
using rtmp_server::relay::RtmpPushClient;

namespace {

bool send_all(int fd, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto sent = ::send(fd, bytes.data() + offset, bytes.size() - offset, 0);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool receive_exact(int fd, std::span<std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto received = ::recv(fd, bytes.data() + offset, bytes.size() - offset, 0);
        if (received > 0) {
            offset += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

RtmpMessage make_command(std::uint32_t stream_id, const std::vector<Amf0Value>& values) {
    RtmpMessage message;
    message.chunk_stream_id = 3;
    message.message_stream_id = stream_id;
    message.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    for (const auto& value : values) protocol::amf0::encode(value, message.payload);
    return message;
}

std::vector<Amf0Value> command_values(const RtmpMessage& message) {
    if (message.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) return {};
    auto values = protocol::amf0::decode_all(message.payload);
    if (!values) return {};
    return values.value();
}

std::string command_name(const RtmpMessage& message) {
    const auto values = command_values(message);
    if (values.empty() || !values[0].is_string()) return {};
    return values[0].as_string();
}

HandoffMessage video(bool keyframe, std::uint32_t timestamp, std::byte marker) {
    HandoffMessage message;
    message.video = true;
    message.keyframe = keyframe;
    message.timestamp = timestamp;
    message.payload = {keyframe ? std::byte{0x17} : std::byte{0x27}, std::byte{0x01}, std::byte{0},
                       std::byte{0}, std::byte{0}, marker};
    return message;
}

// A minimal RTMP ingest: completes the handshake, answers connect and
// createStream, accepts the publish and records the media messages it is sent.
// This is what a relay target (another origin) or a CDN ingest looks like from
// the push client's side of the wire.
class FakeIngest {
public:
    FakeIngest() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 1);
        socklen_t size = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
    }

    ~FakeIngest() {
        stop();
        if (listener_ >= 0) ::close(listener_);
    }

    void start(bool reject_publish = false) {
        thread_ = std::jthread([this, reject_publish] { serve(reject_publish); });
    }

    void stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::string url(std::string_view path) const {
        return "rtmp://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    [[nodiscard]] std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }
    [[nodiscard]] std::vector<RtmpMessage> media() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return media_;
    }
    [[nodiscard]] std::string publish_name() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return publish_name_;
    }
    [[nodiscard]] std::string tc_url() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tc_url_;
    }

    [[nodiscard]] bool wait_for_media(std::size_t count,
                                      std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (media_.size() >= count) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

private:
    void serve(bool reject_publish) {
        const int peer = ::accept(listener_, nullptr, nullptr);
        if (peer < 0) return;

        std::array<std::byte, 1 + protocol::handshake::kHandshakeChunkSize> c0c1{};
        if (!receive_exact(peer, c0c1)) {
            ::close(peer);
            return;
        }
        std::vector<std::byte> handshake(1 + 2 * protocol::handshake::kHandshakeChunkSize,
                                         std::byte{0});
        handshake[0] = static_cast<std::byte>(protocol::handshake::kRtmpVersion);
        std::copy(c0c1.begin() + 1, c0c1.end(),
                  handshake.begin() + 1 + protocol::handshake::kHandshakeChunkSize);
        send_all(peer, handshake);

        std::array<std::byte, protocol::handshake::kHandshakeChunkSize> c2{};
        if (!receive_exact(peer, c2)) {
            ::close(peer);
            return;
        }

        protocol::chunk::ChunkEncoder encoder;
        protocol::chunk::ChunkDecoder decoder(1024 * 1024);
        const auto send_message = [&](const RtmpMessage& message) {
            std::vector<std::byte> bytes;
            encoder.encode_message(message, bytes);
            send_all(peer, bytes);
        };
        decoder.set_message_handler([&](RtmpMessage message) {
            const auto type = static_cast<MessageTypeId>(message.message_type_id);
            if (type == MessageTypeId::Video || type == MessageTypeId::Audio ||
                type == MessageTypeId::Amf0Data) {
                std::lock_guard<std::mutex> lock(mutex_);
                media_.push_back(std::move(message));
                return;
            }
            const std::string name = command_name(message);
            if (name.empty()) return;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                commands_.push_back(name);
            }
            if (name == "connect") {
                const auto values = command_values(message);
                if (values.size() >= 3) {
                    if (const auto* url = values[2].find("tcUrl"); url && url->is_string()) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        tc_url_ = url->as_string();
                    }
                }
                send_message(make_command(0, {Amf0Value::string("_result"), Amf0Value::number(1),
                                              Amf0Value::object(), Amf0Value::object()}));
            } else if (name == "createStream") {
                send_message(make_command(0, {Amf0Value::string("_result"), Amf0Value::number(4),
                                              Amf0Value::null(), Amf0Value::number(1)}));
            } else if (name == "publish") {
                const auto values = command_values(message);
                if (values.size() >= 4 && values[3].is_string()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    publish_name_ = values[3].as_string();
                }
                const char* code = reject_publish ? "NetStream.Publish.BadName"
                                                  : "NetStream.Publish.Start";
                send_message(make_command(
                    1, {Amf0Value::string("onStatus"), Amf0Value::number(0), Amf0Value::null(),
                        Amf0Value::object({{"code", Amf0Value::string(code)}})}));
            }
        });

        std::array<std::byte, 8192> input{};
        while (running_.load()) {
            pollfd item{peer, POLLIN, 0};
            const int polled = ::poll(&item, 1, 50);
            if (polled <= 0) continue;
            const auto received = ::recv(peer, input.data(), input.size(), 0);
            if (received > 0) {
                decoder.on_bytes_received(
                    std::span<const std::byte>(input.data(), static_cast<std::size_t>(received)));
                continue;
            }
            if (received < 0 && errno == EINTR) continue;
            break;
        }
        ::close(peer);
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{true};
    std::jthread thread_;
    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
    std::vector<RtmpMessage> media_;
    std::string publish_name_;
    std::string tc_url_;
};

} // namespace

TEST(RtmpPushClientTest, CompletesThePublishHandshakeAndForwardsMedia) {
    FakeIngest ingest;
    ingest.start();

    MediaHandoffQueue queue;
    queue.push(video(true, 1000, std::byte{0xA1}));
    queue.push(video(false, 1033, std::byte{0xA2}));

    RtmpPushClient client(ingest.url("/live/secret-key"));
    std::atomic<bool> publishing{false};
    std::jthread pusher([&] {
        auto result = client.run(
            [&] { return !ingest.wait_for_media(2, std::chrono::milliseconds(1)); }, queue,
            [] { return std::vector<HandoffMessage>{}; }, [&] { publishing.store(true); });
        (void)result;
    });

    ASSERT_TRUE(ingest.wait_for_media(2));
    pusher.request_stop();
    queue.close();
    pusher.join();
    ingest.stop();

    const auto commands = ingest.commands();
    // FMLE's preamble, which several CDN ingests require before a publish.
    EXPECT_NE(std::ranges::find(commands, "connect"), commands.end());
    EXPECT_NE(std::ranges::find(commands, "releaseStream"), commands.end());
    EXPECT_NE(std::ranges::find(commands, "FCPublish"), commands.end());
    EXPECT_NE(std::ranges::find(commands, "createStream"), commands.end());
    EXPECT_NE(std::ranges::find(commands, "publish"), commands.end());

    // The stream key is the publish name, and the application is the tcUrl.
    EXPECT_EQ(ingest.publish_name(), "secret-key");
    EXPECT_TRUE(ingest.tc_url().ends_with("/live"));
    EXPECT_TRUE(publishing.load());

    const auto media = ingest.media();
    ASSERT_GE(media.size(), 2u);
    EXPECT_EQ(media[0].message_type_id, static_cast<std::uint8_t>(MessageTypeId::Video));
    EXPECT_EQ(media[0].payload.back(), std::byte{0xA1});
    EXPECT_EQ(media[1].payload.back(), std::byte{0xA2});
    EXPECT_GT(client.bytes_sent(), 0u);
}

// The target must see a stream that starts near zero, however long this server
// has been publishing before the target was attached.
TEST(RtmpPushClientTest, RebasesTimestampsOntoTheFirstForwardedFrame) {
    FakeIngest ingest;
    ingest.start();

    MediaHandoffQueue queue;
    queue.push(video(true, 5'000'000, std::byte{0xB1}));
    queue.push(video(false, 5'000'040, std::byte{0xB2}));

    RtmpPushClient client(ingest.url("/live/key"));
    std::jthread pusher([&] {
        (void)client.run([&] { return !ingest.wait_for_media(2, std::chrono::milliseconds(1)); },
                         queue, [] { return std::vector<HandoffMessage>{}; });
    });
    ASSERT_TRUE(ingest.wait_for_media(2));
    pusher.request_stop();
    queue.close();
    pusher.join();
    ingest.stop();

    const auto media = ingest.media();
    ASSERT_GE(media.size(), 2u);
    EXPECT_EQ(media[0].timestamp, 0u);
    EXPECT_EQ(media[1].timestamp, 40u);
}

// A target that joins mid-stream cannot decode anything without the sequence
// headers, so they are replayed onto every new connection ahead of the media.
TEST(RtmpPushClientTest, SendsPrimingMessagesBeforeAnyMedia) {
    FakeIngest ingest;
    ingest.start();

    MediaHandoffQueue queue;
    queue.push(video(true, 1000, std::byte{0xC9}));

    HandoffMessage sequence_header;
    sequence_header.video = true;
    sequence_header.sequence_header = true;
    sequence_header.payload = {std::byte{0x17}, std::byte{0x00}, std::byte{0},
                               std::byte{0},    std::byte{0},    std::byte{0x01}};

    RtmpPushClient client(ingest.url("/live/key"));
    std::jthread pusher([&] {
        (void)client.run([&] { return !ingest.wait_for_media(2, std::chrono::milliseconds(1)); },
                         queue, [&] { return std::vector<HandoffMessage>{sequence_header}; });
    });
    ASSERT_TRUE(ingest.wait_for_media(2));
    pusher.request_stop();
    queue.close();
    pusher.join();
    ingest.stop();

    const auto media = ingest.media();
    ASSERT_GE(media.size(), 2u);
    EXPECT_EQ(media[0].payload[1], std::byte{0x00}); // AVC sequence header first
    EXPECT_EQ(media[1].payload.back(), std::byte{0xC9});
}

// Everything before the first keyframe of a connection is undecodable on the
// far side, so it is skipped rather than sent as garbage.
TEST(RtmpPushClientTest, WaitsForAKeyframeBeforeForwardingPictures) {
    FakeIngest ingest;
    ingest.start();

    MediaHandoffQueue queue;
    queue.push(video(false, 1000, std::byte{0xD1})); // mid-GOP, must be skipped
    queue.push(video(true, 1033, std::byte{0xD2}));
    queue.push(video(false, 1066, std::byte{0xD3}));

    RtmpPushClient client(ingest.url("/live/key"));
    std::jthread pusher([&] {
        (void)client.run([&] { return !ingest.wait_for_media(2, std::chrono::milliseconds(1)); },
                         queue, [] { return std::vector<HandoffMessage>{}; });
    });
    ASSERT_TRUE(ingest.wait_for_media(2));
    pusher.request_stop();
    queue.close();
    pusher.join();
    ingest.stop();

    const auto media = ingest.media();
    ASSERT_GE(media.size(), 2u);
    EXPECT_EQ(media[0].payload.back(), std::byte{0xD2});
    EXPECT_EQ(media[1].payload.back(), std::byte{0xD3});
}

TEST(RtmpPushClientTest, ReportsARejectedPublishInsteadOfStreamingIntoTheVoid) {
    FakeIngest ingest;
    ingest.start(/*reject_publish=*/true);

    MediaHandoffQueue queue;
    queue.push(video(true, 1000, std::byte{0xE1}));

    RtmpPushClient client(ingest.url("/live/wrong-key"));
    auto result = client.run([] { return true; }, queue,
                             [] { return std::vector<HandoffMessage>{}; });
    ingest.stop();

    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("refused the publish"), std::string::npos);
}

TEST(RtmpPushClientTest, RejectsAUrlThatIsNotAPublishableRtmpTarget) {
    MediaHandoffQueue queue;
    RtmpPushClient client("https://example.com/live/key");
    auto result = client.run([] { return true; }, queue,
                             [] { return std::vector<HandoffMessage>{}; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidConfiguration);
}
