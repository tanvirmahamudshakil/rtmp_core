#include <gtest/gtest.h>

#ifdef RTMP_NATIVE_TRANSCODE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"
#include "rtmp_server/transcoding/native/rtmp_source_client.hpp"

using namespace rtmp_server;
using namespace rtmp_server::transcoding::native;

namespace {

using protocol::amf0::Amf0Value;
using protocol::chunk::MessageTypeId;
using protocol::chunk::RtmpMessage;

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

std::string command_name(const RtmpMessage& message) {
    if (message.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) return {};
    auto values = protocol::amf0::decode_all(message.payload);
    if (!values || values.value().empty() || !values.value()[0].is_string()) return {};
    return values.value()[0].as_string();
}

} // namespace

TEST(RtmpSourceClientTest, CompletesNativeHandshakePlayAndReceivesMedia) {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    int reuse = 1;
    ASSERT_EQ(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)), 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);
    socklen_t address_size = sizeof(address);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size), 0);

    std::atomic<bool> media_received{false};
    std::atomic<bool> server_ok{true};
    std::jthread origin([&] {
        const int peer = ::accept(listener, nullptr, nullptr);
        if (peer < 0) {
            server_ok.store(false);
            return;
        }

        std::array<std::byte, 1 + protocol::handshake::kHandshakeChunkSize> c0c1{};
        if (!receive_exact(peer, c0c1)) {
            server_ok.store(false);
            ::close(peer);
            return;
        }
        std::vector<std::byte> handshake(
            1 + 2 * protocol::handshake::kHandshakeChunkSize, std::byte{0});
        handshake[0] = static_cast<std::byte>(protocol::handshake::kRtmpVersion);
        std::copy(c0c1.begin() + 1, c0c1.end(),
                  handshake.begin() + 1 + protocol::handshake::kHandshakeChunkSize);
        if (!send_all(peer, handshake)) server_ok.store(false);

        std::array<std::byte, protocol::handshake::kHandshakeChunkSize> c2{};
        if (!receive_exact(peer, c2)) server_ok.store(false);

        bool media_sent = false;
        protocol::chunk::ChunkEncoder encoder;
        protocol::chunk::ChunkDecoder decoder(1024 * 1024);
        auto send_message = [&](const RtmpMessage& message) {
            std::vector<std::byte> bytes;
            encoder.encode_message(message, bytes);
            if (!send_all(peer, bytes)) server_ok.store(false);
        };
        decoder.set_error_handler([&](core::Error) { server_ok.store(false); });
        decoder.set_message_handler([&](RtmpMessage message) {
            const std::string name = command_name(message);
            if (name == "connect") {
                send_message(make_command(
                    0, {Amf0Value::string("_result"), Amf0Value::number(1),
                        Amf0Value::object(), Amf0Value::object()}));
            } else if (name == "createStream") {
                send_message(make_command(
                    0, {Amf0Value::string("_result"), Amf0Value::number(2),
                        Amf0Value::null(), Amf0Value::number(1)}));
            } else if (name == "play") {
                send_message(make_command(
                    1, {Amf0Value::string("onStatus"), Amf0Value::number(0),
                        Amf0Value::null(),
                        Amf0Value::object({{"code", Amf0Value::string("NetStream.Play.Start")}})}));
                RtmpMessage video;
                video.chunk_stream_id = 6;
                video.message_stream_id = 1;
                video.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Video);
                video.timestamp = 42;
                video.payload = {std::byte{0x17}, std::byte{0x01}, std::byte{0},
                                 std::byte{0}, std::byte{0}, std::byte{0xAA}};
                send_message(video);
                media_sent = true;
            }
        });

        std::array<std::byte, 4096> input{};
        while (!media_sent && server_ok.load()) {
            const auto received = ::recv(peer, input.data(), input.size(), 0);
            if (received > 0) {
                decoder.on_bytes_received(
                    std::span<const std::byte>(input.data(), static_cast<std::size_t>(received)));
            } else if (received < 0 && errno == EINTR) {
                continue;
            } else {
                server_ok.store(false);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ::close(peer);
    });

    RtmpSourceClient client("rtmp://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) +
                            "/live/camera?token=ok");
    auto result = client.run(
        [&] { return !media_received.load(); },
        [&](const RtmpMessage& message) -> core::Result<void> {
            if (message.message_type_id == static_cast<std::uint8_t>(MessageTypeId::Video) &&
                message.timestamp == 42 && message.payload.size() == 6) {
                media_received.store(true);
            }
            return {};
        });

    EXPECT_TRUE(result.ok()) << (result.ok() ? "" : result.error().message());
    EXPECT_TRUE(media_received.load());
    EXPECT_TRUE(server_ok.load());
    ::close(listener);
}

#endif
