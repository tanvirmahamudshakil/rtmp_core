// Phase 1 integration tests (docs/v2_promot.md "PHASE 1 — Complete RTMP
// connection and session pipeline", "Required tests"). These feed *real RTMP
// byte sequences* — handshake bytes, chunk-encoded AMF0 commands, raw
// fragments — into RtmpConnectionSession exactly as
// IoUringEventLoop::start_rtmp_session wires a real TcpConnection's
// receive_handler_ to do in production (src/io/io_uring/event_loop.cpp).
//
// RtmpConnectionSession itself is transport-agnostic (no socket, no
// io_uring — see rtmp_connection_session.hpp), which is what makes these
// tests runnable on any platform, including this macOS build host where the
// io_uring transport target is unavailable (CMAKE_SYSTEM_NAME STREQUAL
// "Linux" guard in the top-level CMakeLists.txt). A second, real-socket
// integration test (rtmp_full_session_socket_test.cpp) proves the same
// pipeline over an actual loopback TCP connection using a plain blocking
// socket harness, without depending on io_uring.

#include "rtmp_server/protocol/session/rtmp_connection_session.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"

namespace rtmp_server::protocol::session {
namespace {

using amf0::Amf0Value;
using chunk::ChunkEncoder;
using chunk::MessageTypeId;
using chunk::RtmpMessage;
using commands::LiveFanout;
using commands::StreamRegistry;
using handshake::HandshakeSession;

RtmpMessage make_command(std::uint32_t message_stream_id, std::vector<Amf0Value> values) {
    RtmpMessage msg;
    msg.chunk_stream_id = 3;
    msg.message_stream_id = message_stream_id;
    msg.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    for (const auto& v : values) amf0::encode(v, msg.payload);
    return msg;
}

std::vector<Amf0Value> decode_command_payload(std::span<const std::byte> payload) {
    auto decoded = amf0::decode_all(payload);
    return decoded.ok() ? decoded.value() : std::vector<Amf0Value>{};
}

// Test harness wrapping RtmpConnectionSession the same way
// IoUringEventLoop::start_rtmp_session wires one up: captures outgoing wire
// bytes and close requests instead of touching a real TcpConnection.
class SessionHarness {
public:
    explicit SessionHarness(std::uint64_t connection_id = 1, std::uint32_t max_message_size = 1024,
                             std::uint32_t output_chunk_size = 128) {
        RtmpConnectionSession::Dependencies deps;
        deps.registry = &registry_;
        deps.live_fanout = &fanout_;
        session_ = std::make_unique<RtmpConnectionSession>(connection_id, deps, max_message_size,
                                                             output_chunk_size);
        session_->set_outgoing_handler(
            [this](std::vector<std::byte> bytes) { outgoing_bytes_.insert(outgoing_bytes_.end(), bytes.begin(), bytes.end()); });
        session_->set_close_handler([this]() { closed_ = true; });
        session_->start();
    }

    RtmpConnectionSession& session() { return *session_; }
    StreamRegistry& registry() { return registry_; }
    LiveFanout& fanout() { return fanout_; }
    [[nodiscard]] bool close_requested() const { return closed_; }
    [[nodiscard]] const std::vector<std::byte>& outgoing_bytes() const { return outgoing_bytes_; }

    // Decodes every complete AMF0 command message out of outgoing_bytes_ via
    // a fresh ChunkDecoder, so assertions don't need to hand-parse chunk
    // framing.
    std::vector<std::vector<Amf0Value>> decode_outgoing_commands() {
        std::vector<std::vector<Amf0Value>> commands;
        chunk::ChunkDecoder decoder(16 * 1024 * 1024);
        decoder.set_message_handler([&](RtmpMessage m) {
            if (m.message_type_id == static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) {
                commands.push_back(decode_command_payload(m.payload));
            }
        });
        decoder.on_bytes_received(outgoing_bytes_);
        return commands;
    }

private:
    StreamRegistry registry_;
    LiveFanout fanout_;
    std::unique_ptr<RtmpConnectionSession> session_;
    std::vector<std::byte> outgoing_bytes_;
    bool closed_ = false;
};

// Encodes an RtmpMessage the same way the real peer (OBS/ffmpeg) would send
// it: through a ChunkEncoder, splitting into chunk_size-sized pieces.
std::vector<std::byte> encode(RtmpMessage message, std::uint32_t chunk_size = 128) {
    ChunkEncoder encoder(chunk_size);
    std::vector<std::byte> out;
    encoder.encode_message(message, out);
    return out;
}

TEST(RtmpConnectionSessionTest, ConnectCommandProducesConnectSuccessResult) {
    SessionHarness harness;
    auto bytes = encode(make_command(
        0, {Amf0Value::string("connect"), Amf0Value::number(1),
            Amf0Value::object({{"app", Amf0Value::string("live")}, {"tcUrl", Amf0Value::string("rtmp://x/live")}})}));

    harness.session().on_bytes_received(bytes);

    auto commands = harness.decode_outgoing_commands();
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0][0].as_string(), "_result");
    EXPECT_EQ(commands[0][3].find("code")->as_string(), "NetConnection.Connect.Success");
    EXPECT_FALSE(harness.close_requested());
}

TEST(RtmpConnectionSessionTest, FragmentedRtmpChunksAreReassembledOneByteAtATime) {
    SessionHarness harness;
    auto bytes = encode(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                          Amf0Value::object({{"app", Amf0Value::string("live")}})}));

    for (std::byte b : bytes) {
        harness.session().on_bytes_received(std::span<const std::byte>(&b, 1));
    }

    auto commands = harness.decode_outgoing_commands();
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0][0].as_string(), "_result");
}

TEST(RtmpConnectionSessionTest, MultipleChunksInOneReceiveBufferAreAllProcessed) {
    SessionHarness harness;
    auto connect_bytes = encode(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                                  Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    auto create_stream_bytes =
        encode(make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));

    std::vector<std::byte> combined = connect_bytes;
    combined.insert(combined.end(), create_stream_bytes.begin(), create_stream_bytes.end());

    harness.session().on_bytes_received(combined);

    auto commands = harness.decode_outgoing_commands();
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0][0].as_string(), "_result"); // connect result
    EXPECT_EQ(commands[1][0].as_string(), "_result"); // createStream result
    EXPECT_GT(commands[1][3].as_number(), 0);
}

TEST(RtmpConnectionSessionTest, CreateStreamPublishAndPlayFullLifecycle) {
    // Publisher side.
    SessionHarness publisher(/*connection_id=*/1);
    publisher.session().on_bytes_received(encode(make_command(
        0, {Amf0Value::string("connect"), Amf0Value::number(1), Amf0Value::object({{"app", Amf0Value::string("live")}})})));
    publisher.session().on_bytes_received(
        encode(make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()})));
    auto create_reply = publisher.decode_outgoing_commands();
    ASSERT_EQ(create_reply.size(), 2u);
    auto pub_stream_id = static_cast<std::uint32_t>(create_reply[1][3].as_number());

    publisher.session().on_bytes_received(encode(make_command(
        pub_stream_id, {Amf0Value::string("publish"), Amf0Value::number(0), Amf0Value::null(),
                        Amf0Value::string("mykey"), Amf0Value::string("live")})));
    auto publish_reply = publisher.decode_outgoing_commands();
    ASSERT_EQ(publish_reply.size(), 3u);
    EXPECT_EQ(publish_reply[2][3].find("code")->as_string(), "NetStream.Publish.Start");
    EXPECT_TRUE(publisher.registry().is_published("mykey"));

    // Viewer side, sharing the same registry/fanout (as the real event loop
    // does — one process-wide StreamRegistry/LiveFanout, one
    // RtmpConnectionSession per connection).
    RtmpConnectionSession::Dependencies deps;
    deps.registry = &publisher.registry();
    deps.live_fanout = &publisher.fanout();
    RtmpConnectionSession viewer_session(/*connection_id=*/2, deps, 1024, 128);
    std::vector<std::byte> viewer_out;
    viewer_session.set_outgoing_handler(
        [&viewer_out](std::vector<std::byte> b) { viewer_out.insert(viewer_out.end(), b.begin(), b.end()); });
    viewer_session.start();

    viewer_session.on_bytes_received(encode(make_command(
        0, {Amf0Value::string("connect"), Amf0Value::number(1), Amf0Value::object({{"app", Amf0Value::string("live")}})})));
    viewer_session.on_bytes_received(
        encode(make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()})));

    chunk::ChunkDecoder decoder(1024);
    std::vector<Amf0Value> last_command;
    decoder.set_message_handler([&](RtmpMessage m) {
        if (m.message_type_id == static_cast<std::uint8_t>(MessageTypeId::Amf0Command))
            last_command = decode_command_payload(m.payload);
    });
    decoder.on_bytes_received(viewer_out);
    auto view_stream_id = static_cast<std::uint32_t>(last_command[3].as_number());
    viewer_out.clear();

    viewer_session.on_bytes_received(encode(make_command(
        view_stream_id, {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
                         Amf0Value::string("mykey")})));

    std::vector<Amf0Value> play_status;
    chunk::ChunkDecoder decoder2(1024);
    decoder2.set_message_handler([&](RtmpMessage m) {
        if (m.message_type_id == static_cast<std::uint8_t>(MessageTypeId::Amf0Command))
            play_status = decode_command_payload(m.payload);
    });
    decoder2.on_bytes_received(viewer_out);
    ASSERT_FALSE(play_status.empty());
    EXPECT_EQ(play_status[0].as_string(), "onStatus");
    EXPECT_EQ(play_status[3].find("code")->as_string(), "NetStream.Play.Start");
}

TEST(RtmpConnectionSessionTest, MalformedAmfPayloadIsDroppedWithoutCrashingOrClosing) {
    SessionHarness harness;
    RtmpMessage bad;
    bad.chunk_stream_id = 3;
    bad.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    bad.payload = {static_cast<std::byte>(0xFF)}; // invalid AMF0 type marker
    harness.session().on_bytes_received(encode(bad));

    EXPECT_TRUE(harness.outgoing_bytes().empty());
    EXPECT_FALSE(harness.close_requested());
    EXPECT_FALSE(harness.session().failed());
}

TEST(RtmpConnectionSessionTest, MessageExceedingConfiguredMaximumClosesConnection) {
    SessionHarness harness(/*connection_id=*/1, /*max_message_size=*/32);

    // A fmt0 header declaring a message length larger than max_message_size
    // (32) must be rejected by ChunkDecoder before any payload is even
    // buffered (Phase 1 task 10/11).
    RtmpMessage oversized;
    oversized.chunk_stream_id = 3;
    oversized.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Data);
    oversized.payload = std::vector<std::byte>(64, std::byte{0});

    // Hand-build a minimal fmt0 chunk header declaring length=64 > max=32,
    // rather than going through ChunkEncoder (which would happily encode
    // it) — the point under test is decoder-side rejection.
    std::vector<std::byte> raw;
    raw.push_back(std::byte{0x03}); // fmt 0, csid 3
    raw.push_back(std::byte{0}); raw.push_back(std::byte{0}); raw.push_back(std::byte{0}); // timestamp
    raw.push_back(std::byte{0}); raw.push_back(std::byte{0}); raw.push_back(std::byte{64}); // length=64 (BE 24-bit)
    raw.push_back(std::byte{static_cast<std::uint8_t>(MessageTypeId::Amf0Data)});
    raw.push_back(std::byte{0}); raw.push_back(std::byte{0}); raw.push_back(std::byte{0}); raw.push_back(std::byte{0}); // stream id

    harness.session().on_bytes_received(raw);

    EXPECT_TRUE(harness.session().failed());
    EXPECT_TRUE(harness.close_requested());
}

TEST(RtmpConnectionSessionTest, PingRequestProducesPingResponseWithSameTimestamp) {
    SessionHarness harness;
    RtmpMessage ping;
    ping.chunk_stream_id = chunk::kProtocolControlChunkStreamId;
    ping.message_stream_id = chunk::kProtocolControlMessageStreamId;
    ping.message_type_id = static_cast<std::uint8_t>(MessageTypeId::UserControlMessage);
    // Event type 6 (PingRequest), 2 bytes, followed by a 4-byte timestamp.
    ping.payload = {std::byte{0}, std::byte{6}, std::byte{0}, std::byte{0}, std::byte{0x01}, std::byte{0x02}};

    harness.session().on_bytes_received(encode(ping));

    chunk::ChunkDecoder decoder(1024);
    bool got_ping_response = false;
    decoder.set_message_handler([&](RtmpMessage m) {
        if (m.message_type_id == static_cast<std::uint8_t>(MessageTypeId::UserControlMessage) &&
            m.payload.size() >= 6) {
            auto event = (static_cast<std::uint8_t>(m.payload[0]) << 8) | static_cast<std::uint8_t>(m.payload[1]);
            if (event == 7) { // PingResponse
                got_ping_response = true;
                EXPECT_EQ(m.payload[4], std::byte{0x01});
                EXPECT_EQ(m.payload[5], std::byte{0x02});
            }
        }
    });
    decoder.on_bytes_received(harness.outgoing_bytes());
    EXPECT_TRUE(got_ping_response);
}

TEST(RtmpConnectionSessionTest, ConnectionCleanupUnregistersPublisherAndSubscriber) {
    SessionHarness publisher;
    publisher.session().on_bytes_received(encode(make_command(
        0, {Amf0Value::string("connect"), Amf0Value::number(1), Amf0Value::object({{"app", Amf0Value::string("live")}})})));
    publisher.session().on_bytes_received(
        encode(make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()})));
    auto replies = publisher.decode_outgoing_commands();
    auto stream_id = static_cast<std::uint32_t>(replies[1][3].as_number());
    publisher.session().on_bytes_received(encode(make_command(
        stream_id, {Amf0Value::string("publish"), Amf0Value::number(0), Amf0Value::null(),
                    Amf0Value::string("mykey"), Amf0Value::string("live")})));
    ASSERT_TRUE(publisher.registry().is_published("mykey"));

    publisher.session().on_connection_closed();

    EXPECT_FALSE(publisher.registry().is_published("mykey"));
}

TEST(RtmpConnectionSessionTest, HandshakeTrailingBytesArePreservedAndReachTheSession) {
    // Reproduces production-gap-analysis item #3: a client (like OBS) that
    // sends `connect` in the same TCP write as C2. HandshakeSession must not
    // discard those trailing bytes, and once fed into RtmpConnectionSession
    // (via take_trailing_bytes(), exactly as
    // IoUringEventLoop::start_handshake now does) they must be processed.
    HandshakeSession handshake;
    bool handshake_completed = false;
    handshake.set_complete_handler([&]() { handshake_completed = true; });

    // Drive the handshake to WaitingForC2 first (server already sent
    // S0/S1/S2 internally via the send handler, which we ignore here).
    handshake.set_send_handler([](core::SharedBuffer) {});
    std::vector<std::byte> c0(1, std::byte{handshake::kRtmpVersion});
    handshake.on_bytes_received(c0);
    std::vector<std::byte> c1(handshake::kHandshakeChunkSize, std::byte{0});
    handshake.on_bytes_received(c1);
    ASSERT_EQ(handshake.state(), handshake::HandshakeState::WaitingForC2);

    // C2 plus a pipelined `connect` command in the exact same
    // on_bytes_received() call.
    SessionHarness harness;
    auto connect_bytes = encode(make_command(
        0, {Amf0Value::string("connect"), Amf0Value::number(1), Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    std::vector<std::byte> c2_plus_connect(handshake::kHandshakeChunkSize, std::byte{0});
    c2_plus_connect.insert(c2_plus_connect.end(), connect_bytes.begin(), connect_bytes.end());

    handshake.on_bytes_received(c2_plus_connect);
    ASSERT_TRUE(handshake_completed);

    auto trailing = handshake.take_trailing_bytes();
    ASSERT_EQ(trailing.size(), connect_bytes.size());

    harness.session().on_bytes_received(trailing);

    auto commands = harness.decode_outgoing_commands();
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0][0].as_string(), "_result");
    EXPECT_EQ(commands[0][3].find("code")->as_string(), "NetConnection.Connect.Success");
}

} // namespace
} // namespace rtmp_server::protocol::session
