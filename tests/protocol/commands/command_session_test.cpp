#include "rtmp_server/protocol/commands/command_session.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"

namespace rtmp_server::protocol::commands {
namespace {

using amf0::Amf0Value;
using chunk::MessageTypeId;
using chunk::RtmpMessage;

RtmpMessage make_command(std::uint32_t message_stream_id, std::vector<Amf0Value> values) {
    RtmpMessage msg;
    msg.chunk_stream_id = 3;
    msg.message_stream_id = message_stream_id;
    msg.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    for (const auto& v : values) amf0::encode(v, msg.payload);
    return msg;
}

// Decodes a captured outgoing RtmpMessage back into its AMF0 values, for
// asserting on response shape without hardcoding byte offsets.
std::vector<Amf0Value> decode_outgoing(const RtmpMessage& msg) {
    auto decoded = amf0::decode_all(msg.payload);
    EXPECT_TRUE(decoded.ok());
    return decoded.ok() ? decoded.value() : std::vector<Amf0Value>{};
}

class CommandSessionTest : public ::testing::Test {
protected:
    StreamRegistry registry;
    std::vector<RtmpMessage> outgoing;

    // Default: only "good-key" is authorized. Individual tests may replace
    // key_validator before constructing the session.
    CommandSession make_session(StreamKeyValidator validator = [](std::string_view, std::string_view key) {
        return key == "good-key";
    }) {
        CommandSession session(/*connection_id=*/42, registry, std::move(validator));
        session.set_outgoing_handler([this](RtmpMessage m) { outgoing.push_back(std::move(m)); });
        return session;
    }
};

TEST_F(CommandSessionTest, ConnectProducesResultWithConnectSuccessStatus) {
    auto session = make_session();
    session.handle_message(make_command(
        0, {Amf0Value::string("connect"), Amf0Value::number(1),
            Amf0Value::object({{"app", Amf0Value::string("live")}, {"tcUrl", Amf0Value::string("rtmp://x/live")}})}));

    ASSERT_EQ(outgoing.size(), 1u);
    auto values = decode_outgoing(outgoing[0]);
    ASSERT_EQ(values.size(), 4u);
    EXPECT_EQ(values[0].as_string(), "_result");
    EXPECT_EQ(values[1].as_number(), 1);
    ASSERT_TRUE(values[3].is_object());
    EXPECT_EQ(values[3].find("code")->as_string(), "NetConnection.Connect.Success");
    EXPECT_EQ(values[3].find("level")->as_string(), "status");
    EXPECT_TRUE(session.is_connected());
    EXPECT_EQ(session.app_name(), "live");
}

TEST_F(CommandSessionTest, CreateStreamProducesResultWithNumericStreamId) {
    auto session = make_session();
    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    outgoing.clear();

    session.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));

    ASSERT_EQ(outgoing.size(), 1u);
    auto values = decode_outgoing(outgoing[0]);
    ASSERT_EQ(values.size(), 4u);
    EXPECT_EQ(values[0].as_string(), "_result");
    EXPECT_EQ(values[1].as_number(), 2);
    ASSERT_TRUE(values[3].is_number());
    EXPECT_GT(values[3].as_number(), 0);
    EXPECT_EQ(session.last_created_stream_id(), static_cast<std::uint32_t>(values[3].as_number()));
}

TEST_F(CommandSessionTest, ValidKeyPublishProducesPublishStartStatus) {
    auto session = make_session();
    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id = session.last_created_stream_id();
    outgoing.clear();

    session.handle_message(make_command(stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                      Amf0Value::null(), Amf0Value::string("good-key"),
                                                      Amf0Value::string("live")}));

    ASSERT_EQ(outgoing.size(), 1u);
    EXPECT_EQ(outgoing[0].message_stream_id, stream_id);
    auto values = decode_outgoing(outgoing[0]);
    ASSERT_EQ(values.size(), 4u);
    EXPECT_EQ(values[0].as_string(), "onStatus");
    ASSERT_TRUE(values[3].is_object());
    EXPECT_EQ(values[3].find("code")->as_string(), "NetStream.Publish.Start");
    EXPECT_EQ(values[3].find("level")->as_string(), "status");
    EXPECT_EQ(session.stream_state(stream_id), NetStreamState::Publishing);
}

TEST_F(CommandSessionTest, InvalidKeyPublishIsRejectedAndDoesNotTransitionToPublishing) {
    auto session = make_session();
    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id = session.last_created_stream_id();
    outgoing.clear();

    session.handle_message(make_command(stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                      Amf0Value::null(), Amf0Value::string("bad-key"),
                                                      Amf0Value::string("live")}));

    ASSERT_EQ(outgoing.size(), 1u);
    auto values = decode_outgoing(outgoing[0]);
    EXPECT_EQ(values[0].as_string(), "onStatus");
    ASSERT_TRUE(values[3].is_object());
    EXPECT_EQ(values[3].find("code")->as_string(), "NetStream.Publish.BadName");
    EXPECT_EQ(values[3].find("level")->as_string(), "error");
    EXPECT_NE(session.stream_state(stream_id), NetStreamState::Publishing);
    EXPECT_FALSE(registry.is_published("bad-key"));
}

TEST_F(CommandSessionTest, PublishedStreamAppearsInRegistry) {
    auto session = make_session();
    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id = session.last_created_stream_id();

    session.handle_message(make_command(stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                      Amf0Value::null(), Amf0Value::string("good-key"),
                                                      Amf0Value::string("live")}));

    ASSERT_TRUE(registry.is_published("good-key"));
    auto reg = registry.find("good-key");
    ASSERT_TRUE(reg.has_value());
    EXPECT_EQ(reg->connection_id, 42u);
    EXPECT_EQ(reg->stream_id, stream_id);
    EXPECT_EQ(reg->app, "live");
}

TEST_F(CommandSessionTest, SecondPublisherForSameKeyIsRejected) {
    auto session1 = make_session();
    session1.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                              Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session1.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id1 = session1.last_created_stream_id();
    session1.handle_message(make_command(stream_id1, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                        Amf0Value::null(), Amf0Value::string("good-key"),
                                                        Amf0Value::string("live")}));
    ASSERT_TRUE(registry.is_published("good-key"));

    std::vector<RtmpMessage> outgoing2;
    CommandSession session2(/*connection_id=*/99, registry,
                             [](std::string_view, std::string_view key) { return key == "good-key"; });
    session2.set_outgoing_handler([&outgoing2](RtmpMessage m) { outgoing2.push_back(std::move(m)); });
    session2.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                              Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session2.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id2 = session2.last_created_stream_id();
    outgoing2.clear();
    session2.handle_message(make_command(stream_id2, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                        Amf0Value::null(), Amf0Value::string("good-key"),
                                                        Amf0Value::string("live")}));

    ASSERT_EQ(outgoing2.size(), 1u);
    auto values = decode_outgoing(outgoing2[0]);
    EXPECT_EQ(values[3].find("code")->as_string(), "NetStream.Publish.BadName");
    EXPECT_NE(session2.stream_state(stream_id2), NetStreamState::Publishing);
    // The original publisher's registration must be untouched.
    auto reg = registry.find("good-key");
    ASSERT_TRUE(reg.has_value());
    EXPECT_EQ(reg->connection_id, 42u);
}

TEST_F(CommandSessionTest, PlayProducesPlayStartStatus) {
    auto session = make_session();
    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id = session.last_created_stream_id();
    outgoing.clear();

    session.handle_message(make_command(
        stream_id, {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
                    Amf0Value::string("good-key")}));

    ASSERT_EQ(outgoing.size(), 1u);
    auto values = decode_outgoing(outgoing[0]);
    EXPECT_EQ(values[0].as_string(), "onStatus");
    EXPECT_EQ(values[3].find("code")->as_string(), "NetStream.Play.Start");
    EXPECT_EQ(session.stream_state(stream_id), NetStreamState::Playing);
}

TEST_F(CommandSessionTest, DeleteStreamRemovesPublisherFromRegistry) {
    auto session = make_session();
    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id = session.last_created_stream_id();
    session.handle_message(make_command(stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                      Amf0Value::null(), Amf0Value::string("good-key"),
                                                      Amf0Value::string("live")}));
    ASSERT_TRUE(registry.is_published("good-key"));

    session.handle_message(make_command(
        0, {Amf0Value::string("deleteStream"), Amf0Value::number(0), Amf0Value::null(),
            Amf0Value::number(stream_id)}));

    EXPECT_FALSE(registry.is_published("good-key"));
}

TEST_F(CommandSessionTest, ConnectionCloseUnregistersPublishedStream) {
    auto session = make_session();
    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id = session.last_created_stream_id();
    session.handle_message(make_command(stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                      Amf0Value::null(), Amf0Value::string("good-key"),
                                                      Amf0Value::string("live")}));
    ASSERT_TRUE(registry.is_published("good-key"));

    session.on_connection_closed();

    EXPECT_FALSE(registry.is_published("good-key"));
}

TEST_F(CommandSessionTest, ReleaseStreamAndFcPublishReplyWithResult) {
    auto session = make_session();
    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    outgoing.clear();

    session.handle_message(make_command(
        0, {Amf0Value::string("releaseStream"), Amf0Value::number(2), Amf0Value::null(),
            Amf0Value::string("good-key")}));
    session.handle_message(make_command(
        0, {Amf0Value::string("FCPublish"), Amf0Value::number(3), Amf0Value::null(),
            Amf0Value::string("good-key")}));

    ASSERT_EQ(outgoing.size(), 2u);
    auto v1 = decode_outgoing(outgoing[0]);
    auto v2 = decode_outgoing(outgoing[1]);
    EXPECT_EQ(v1[0].as_string(), "_result");
    EXPECT_EQ(v1[1].as_number(), 2);
    EXPECT_EQ(v2[0].as_string(), "_result");
    EXPECT_EQ(v2[1].as_number(), 3);
}

TEST_F(CommandSessionTest, NonAmf0CommandMessagesAreIgnored) {
    auto session = make_session();
    RtmpMessage data_msg;
    data_msg.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Data);
    amf0::encode(Amf0Value::string("@setDataFrame"), data_msg.payload);
    session.handle_message(data_msg);
    EXPECT_TRUE(outgoing.empty());
}

TEST_F(CommandSessionTest, MalformedCommandPayloadIsDroppedWithoutCrashing) {
    auto session = make_session();
    RtmpMessage msg;
    msg.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    msg.payload = {static_cast<std::byte>(0xFF)}; // invalid AMF0 marker
    session.handle_message(msg);
    EXPECT_TRUE(outgoing.empty());
}

} // namespace
} // namespace rtmp_server::protocol::commands
