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

TEST_F(CommandSessionTest, AudioVideoMetadataAreRoutedToMediaIngestOnlyWhilePublishing) {
    media::MediaIngest ingest;
    auto session = make_session();
    session.set_media_ingest(&ingest);

    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id = session.last_created_stream_id();

    // Before publish(): audio/video/metadata on this stream ID must NOT
    // reach MediaIngest.
    RtmpMessage early_audio;
    early_audio.message_stream_id = stream_id;
    early_audio.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Audio);
    early_audio.payload = {static_cast<std::byte>(0xAF), static_cast<std::byte>(0x01)};
    session.handle_message(early_audio);
    EXPECT_EQ(ingest.stream_count(), 0u);

    session.handle_message(make_command(stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                      Amf0Value::null(), Amf0Value::string("good-key"),
                                                      Amf0Value::string("live")}));

    RtmpMessage audio;
    audio.message_stream_id = stream_id;
    audio.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Audio);
    audio.payload = {static_cast<std::byte>(0xAF), static_cast<std::byte>(0x01), static_cast<std::byte>(0x00)};
    audio.timestamp = 5;
    session.handle_message(audio);

    ASSERT_EQ(ingest.stream_count(), 1u);
    const auto* state = ingest.find("good-key");
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->stats.audio_message_count, 1u);
    EXPECT_EQ(state->stats.last_audio_timestamp, 5u);
}

// Minimal RecorderSink spy — the protocol test avoids linking the recording
// library by implementing the abstract hook inline (same shape the real
// recording::Recorder implements).
class RecorderSpy : public RecorderSink {
public:
    int audio = 0;
    int video = 0;
    int metadata = 0;
    int finalized = 0;
    void on_audio(const chunk::RtmpMessage&) override { ++audio; }
    void on_video(const chunk::RtmpMessage&) override { ++video; }
    void on_metadata(const chunk::RtmpMessage&) override { ++metadata; }
    void finalize() override { ++finalized; }
};

TEST_F(CommandSessionTest, MediaIsRoutedToRecorderWhilePublishingAndFinalizedOnClose) {
    RecorderSpy recorder;
    auto session = make_session();
    session.set_recorder(&recorder);

    session.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                             Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    session.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t stream_id = session.last_created_stream_id();

    // Before publish(): media must not reach the recorder.
    RtmpMessage early_video;
    early_video.message_stream_id = stream_id;
    early_video.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Video);
    early_video.payload = {static_cast<std::byte>(0x17)};
    session.handle_message(early_video);
    EXPECT_EQ(recorder.video, 0);

    session.handle_message(make_command(stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                      Amf0Value::null(), Amf0Value::string("good-key"),
                                                      Amf0Value::string("live")}));

    RtmpMessage video = early_video;
    session.handle_message(video);
    EXPECT_EQ(recorder.video, 1);

    // Connection close finalizes the recording exactly once.
    session.on_connection_closed();
    EXPECT_EQ(recorder.finalized, 1);
}

RtmpMessage make_media(std::uint32_t stream_id, MessageTypeId type, std::vector<std::byte> payload,
                       std::uint32_t timestamp = 0) {
    RtmpMessage m;
    m.message_stream_id = stream_id;
    m.message_type_id = static_cast<std::uint8_t>(type);
    m.payload = std::move(payload);
    m.timestamp = timestamp;
    return m;
}

TEST_F(CommandSessionTest, OneViewerReceivesFannedOutMedia) {
    LiveFanout fanout;
    auto publisher = make_session();
    publisher.set_live_fanout(&fanout);
    publisher.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                               Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    publisher.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t pub_stream_id = publisher.last_created_stream_id();
    publisher.handle_message(make_command(pub_stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                            Amf0Value::null(), Amf0Value::string("good-key"),
                                                            Amf0Value::string("live")}));

    std::vector<RtmpMessage> viewer_outgoing;
    CommandSession viewer(/*connection_id=*/7, registry,
                           [](std::string_view, std::string_view) { return true; });
    viewer.set_outgoing_handler([&viewer_outgoing](RtmpMessage m) { viewer_outgoing.push_back(std::move(m)); });
    viewer.set_live_fanout(&fanout);
    viewer.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                            Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    viewer.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t view_stream_id = viewer.last_created_stream_id();
    viewer.handle_message(make_command(
        view_stream_id, {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
                          Amf0Value::string("good-key")}));
    viewer_outgoing.clear(); // drop the NetStream.Play.Start reply, only care about media below

    // A keyframe published after the viewer subscribed must reach it live.
    publisher.handle_message(make_media(pub_stream_id, MessageTypeId::Video,
                                         {static_cast<std::byte>(0x17), static_cast<std::byte>(0x01)}));

    ASSERT_EQ(viewer_outgoing.size(), 1u);
    EXPECT_EQ(viewer_outgoing[0].message_type_id, static_cast<std::uint8_t>(MessageTypeId::Video));
    EXPECT_EQ(viewer_outgoing[0].message_stream_id, view_stream_id);
}

TEST_F(CommandSessionTest, MultipleViewersEachReceiveFannedOutMedia) {
    LiveFanout fanout;
    auto publisher = make_session();
    publisher.set_live_fanout(&fanout);
    publisher.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                               Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    publisher.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t pub_stream_id = publisher.last_created_stream_id();
    publisher.handle_message(make_command(pub_stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                            Amf0Value::null(), Amf0Value::string("good-key"),
                                                            Amf0Value::string("live")}));

    auto subscribe_viewer = [&](std::uint64_t connection_id, std::vector<RtmpMessage>& outgoing_out) {
        auto* viewer = new CommandSession(connection_id, registry,
                                           [](std::string_view, std::string_view) { return true; });
        viewer->set_outgoing_handler([&outgoing_out](RtmpMessage m) { outgoing_out.push_back(std::move(m)); });
        viewer->set_live_fanout(&fanout);
        viewer->handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                                 Amf0Value::object({{"app", Amf0Value::string("live")}})}));
        viewer->handle_message(
            make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
        std::uint32_t stream_id = viewer->last_created_stream_id();
        viewer->handle_message(make_command(
            stream_id, {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
                        Amf0Value::string("good-key")}));
        outgoing_out.clear(); // drop the Play.Start reply
        return viewer;
    };

    std::vector<RtmpMessage> viewer1_outgoing, viewer2_outgoing, viewer3_outgoing;
    std::unique_ptr<CommandSession> viewer1(subscribe_viewer(7, viewer1_outgoing));
    std::unique_ptr<CommandSession> viewer2(subscribe_viewer(8, viewer2_outgoing));
    std::unique_ptr<CommandSession> viewer3(subscribe_viewer(9, viewer3_outgoing));
    ASSERT_EQ(fanout.subscriber_count("good-key"), 3u);

    publisher.handle_message(make_media(pub_stream_id, MessageTypeId::Video,
                                         {static_cast<std::byte>(0x17), static_cast<std::byte>(0x01)}));

    for (const auto* outgoing_ptr : {&viewer1_outgoing, &viewer2_outgoing, &viewer3_outgoing}) {
        ASSERT_EQ(outgoing_ptr->size(), 1u);
        EXPECT_EQ((*outgoing_ptr)[0].message_type_id, static_cast<std::uint8_t>(MessageTypeId::Video));
    }
}

TEST_F(CommandSessionTest, NewViewerReceivesCachedGopAndSequenceHeaders) {
    LiveFanout fanout;
    auto publisher = make_session();
    publisher.set_live_fanout(&fanout);
    publisher.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                               Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    publisher.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t pub_stream_id = publisher.last_created_stream_id();
    publisher.handle_message(make_command(pub_stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                            Amf0Value::null(), Amf0Value::string("good-key"),
                                                            Amf0Value::string("live")}));

    // AVC sequence header, then a keyframe, then an interframe — all before
    // any viewer subscribes.
    publisher.handle_message(make_media(
        pub_stream_id, MessageTypeId::Video,
        {static_cast<std::byte>(0x17), static_cast<std::byte>(0x00), static_cast<std::byte>(0xAA)}));
    publisher.handle_message(make_media(
        pub_stream_id, MessageTypeId::Video,
        {static_cast<std::byte>(0x17), static_cast<std::byte>(0x01), static_cast<std::byte>(0xBB)}));
    publisher.handle_message(make_media(
        pub_stream_id, MessageTypeId::Video,
        {static_cast<std::byte>(0x27), static_cast<std::byte>(0x01), static_cast<std::byte>(0xCC)}));

    std::vector<RtmpMessage> viewer_outgoing;
    CommandSession viewer(/*connection_id=*/7, registry,
                           [](std::string_view, std::string_view) { return true; });
    viewer.set_outgoing_handler([&viewer_outgoing](RtmpMessage m) { viewer_outgoing.push_back(std::move(m)); });
    viewer.set_live_fanout(&fanout);
    viewer.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                            Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    viewer.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t view_stream_id = viewer.last_created_stream_id();
    viewer_outgoing.clear(); // drop connect/createStream replies
    viewer.handle_message(make_command(
        view_stream_id, {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
                          Amf0Value::string("good-key")}));

    // Expect: onStatus (Play.Start), then AVC sequence header, then the two
    // cached GOP video frames (keyframe, interframe) — in that order.
    ASSERT_EQ(viewer_outgoing.size(), 4u);
    EXPECT_EQ(viewer_outgoing[1].message_type_id, static_cast<std::uint8_t>(MessageTypeId::Video));
    EXPECT_EQ(viewer_outgoing[1].payload[1], static_cast<std::byte>(0x00)); // sequence header
    EXPECT_EQ(viewer_outgoing[2].payload[1], static_cast<std::byte>(0x01));
    EXPECT_EQ(viewer_outgoing[2].payload[2], static_cast<std::byte>(0xBB)); // keyframe
    EXPECT_EQ(viewer_outgoing[3].payload[2], static_cast<std::byte>(0xCC)); // interframe
}

TEST_F(CommandSessionTest, PublisherDisconnectEndsViewerSessionCleanly) {
    LiveFanout fanout;
    auto publisher = make_session();
    publisher.set_live_fanout(&fanout);
    publisher.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                               Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    publisher.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t pub_stream_id = publisher.last_created_stream_id();
    publisher.handle_message(make_command(pub_stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                            Amf0Value::null(), Amf0Value::string("good-key"),
                                                            Amf0Value::string("live")}));

    std::vector<RtmpMessage> viewer_outgoing;
    CommandSession viewer(/*connection_id=*/7, registry,
                           [](std::string_view, std::string_view) { return true; });
    viewer.set_outgoing_handler([&viewer_outgoing](RtmpMessage m) { viewer_outgoing.push_back(std::move(m)); });
    viewer.set_live_fanout(&fanout);
    viewer.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                            Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    viewer.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t view_stream_id = viewer.last_created_stream_id();
    viewer.handle_message(make_command(
        view_stream_id, {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
                          Amf0Value::string("good-key")}));
    ASSERT_EQ(fanout.subscriber_count("good-key"), 1u);
    viewer_outgoing.clear();

    publisher.on_connection_closed();

    EXPECT_EQ(fanout.subscriber_count("good-key"), 0u);
    EXPECT_NE(viewer.stream_state(view_stream_id), NetStreamState::Playing);
    ASSERT_EQ(viewer_outgoing.size(), 1u);
    auto values = decode_outgoing(viewer_outgoing[0]);
    EXPECT_EQ(values[0].as_string(), "onStatus");
    EXPECT_EQ(values[3].find("code")->as_string(), "NetStream.Play.UnpublishNotify");
}

TEST_F(CommandSessionTest, ViewerDisconnectRemovesItFromFanoutSubscriberList) {
    LiveFanout fanout;
    auto publisher = make_session();
    publisher.set_live_fanout(&fanout);
    publisher.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                               Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    publisher.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t pub_stream_id = publisher.last_created_stream_id();
    publisher.handle_message(make_command(pub_stream_id, {Amf0Value::string("publish"), Amf0Value::number(0),
                                                            Amf0Value::null(), Amf0Value::string("good-key"),
                                                            Amf0Value::string("live")}));

    CommandSession viewer(/*connection_id=*/7, registry,
                           [](std::string_view, std::string_view) { return true; });
    viewer.set_outgoing_handler([](RtmpMessage) {});
    viewer.set_live_fanout(&fanout);
    viewer.handle_message(make_command(0, {Amf0Value::string("connect"), Amf0Value::number(1),
                                            Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    viewer.handle_message(
        make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    std::uint32_t view_stream_id = viewer.last_created_stream_id();
    viewer.handle_message(make_command(
        view_stream_id, {Amf0Value::string("play"), Amf0Value::number(0), Amf0Value::null(),
                          Amf0Value::string("good-key")}));
    ASSERT_EQ(fanout.subscriber_count("good-key"), 1u);

    viewer.on_connection_closed();

    EXPECT_EQ(fanout.subscriber_count("good-key"), 0u);
}

// Sink that always reports failure, to exercise LiveFanout's slow-client
// eviction independent of CommandSession's own byte-budget policy.
class AlwaysDropsSink : public PlaybackSink {
public:
    int evicted = 0;
    bool on_audio(const chunk::RtmpMessage&) override { return false; }
    bool on_video(const chunk::RtmpMessage&) override { return false; }
    bool on_metadata(const chunk::RtmpMessage&) override { return false; }
    void on_publisher_stopped() override {}
    void on_slow_client_evicted() override { ++evicted; }
};

TEST(LiveFanoutTest, SlowClientIsEvictedAfterMaxConsecutiveDrops) {
    LiveFanout fanout(/*max_consecutive_drops=*/3);
    AlwaysDropsSink sink;
    fanout.subscribe("good-key", /*subscriber_id=*/1, &sink);

    for (int i = 0; i < 3; ++i) {
        RtmpMessage m;
        m.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Video);
        m.payload = {static_cast<std::byte>(0x27), static_cast<std::byte>(0x01)};
        fanout.on_video("good-key", m);
    }

    EXPECT_EQ(sink.evicted, 1);
    EXPECT_EQ(fanout.subscriber_count("good-key"), 0u);
}

} // namespace
} // namespace rtmp_server::protocol::commands
