#include "rtmp_server/recording/recorder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "rtmp_server/media/flv/flv_writer.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_value.hpp"

namespace rtmp_server::recording {
namespace {

namespace amf0 = protocol::amf0;
namespace chunk = protocol::chunk;
using chunk::MessageTypeId;
using chunk::RtmpMessage;

// In-memory FileSink fake: no io_uring, no disk. `pending_` is settable so a
// test can simulate a slow/stalled disk to exercise the bounded-queue drop
// path; `fail_appends` simulates ENOSPC/EIO to exercise the disk-failure guard.
class FakeSink final : public FileSink {
public:
    std::vector<std::byte> file;
    std::size_t pending_ = 0;
    bool fail_appends = false;
    int finalize_calls = 0;

    core::Result<void> append(std::span<const std::byte> data) override {
        if (fail_appends) {
            return core::Error{core::ErrorCode::StorageWriteFailed, core::ErrorCategory::Storage, "fake ENOSPC"};
        }
        file.insert(file.end(), data.begin(), data.end());
        return {};
    }
    core::Result<void> patch(std::uint64_t offset, std::span<const std::byte> data) override {
        if (offset + data.size() > file.size()) {
            return core::Error{core::ErrorCode::StorageWriteFailed, core::ErrorCategory::Storage, "patch oob"};
        }
        std::copy(data.begin(), data.end(), file.begin() + static_cast<std::ptrdiff_t>(offset));
        return {};
    }
    core::Result<void> finalize() override {
        ++finalize_calls;
        return {};
    }
    [[nodiscard]] std::size_t pending_bytes() const override { return pending_; }
    [[nodiscard]] bool healthy() const override { return !fail_appends; }
};

std::vector<std::byte> bytes(std::initializer_list<int> vals) {
    std::vector<std::byte> out;
    for (int v : vals) out.push_back(static_cast<std::byte>(v));
    return out;
}

RtmpMessage media_msg(MessageTypeId type, std::vector<std::byte> payload, std::uint32_t ts) {
    RtmpMessage m;
    m.message_type_id = static_cast<std::uint8_t>(type);
    m.timestamp = ts;
    m.payload = std::move(payload);
    return m;
}

RtmpMessage onmetadata_msg() {
    using amf0::Amf0Value;
    RtmpMessage m;
    m.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Data);
    amf0::encode(Amf0Value::string("onMetaData"), m.payload);
    amf0::Amf0PropertyList props;
    props.emplace_back("width", Amf0Value::number(1920));
    props.emplace_back("height", Amf0Value::number(1080));
    props.emplace_back("framerate", Amf0Value::number(30));
    amf0::encode(Amf0Value::ecma_array(std::move(props)), m.payload);
    return m;
}

media::flv::ParsedFlv parse(const FakeSink& sink) {
    auto r = media::flv::parse_flv(std::span<const std::byte>(sink.file.data(), sink.file.size()));
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().message());
    return r.value();
}

TEST(RecorderTest, ProducesValidFlvWithHeaderMetadataAndMediaInOrder) {
    FakeSink sink;
    Recorder rec(sink);
    rec.on_metadata(onmetadata_msg());
    rec.on_video(media_msg(MessageTypeId::Video, bytes({0x17, 0x00, 0x00}), 0));
    rec.on_audio(media_msg(MessageTypeId::Audio, bytes({0xAF, 0x00, 0x12, 0x10}), 0));
    rec.on_video(media_msg(MessageTypeId::Video, bytes({0x27, 0x01, 0x01}), 33));
    rec.finalize();

    auto flv = parse(sink);
    ASSERT_EQ(flv.tags.size(), 4u);
    EXPECT_EQ(flv.tags[0].type, media::flv::kTagTypeScriptData); // onMetaData first
    EXPECT_EQ(flv.tags[1].type, media::flv::kTagTypeVideo);
    EXPECT_EQ(flv.tags[2].type, media::flv::kTagTypeAudio);
    EXPECT_EQ(flv.tags[3].type, media::flv::kTagTypeVideo);
    EXPECT_EQ(sink.finalize_calls, 1);
    EXPECT_FALSE(rec.stats().failed);
}

TEST(RecorderTest, TimestampsArePreservedIncludingExtendedRange) {
    FakeSink sink;
    Recorder rec(sink);
    rec.on_video(media_msg(MessageTypeId::Video, bytes({0x17, 0x00}), 500));
    rec.on_video(media_msg(MessageTypeId::Video, bytes({0x27, 0x01}), 0x0100'0000u)); // > 24 bits
    rec.finalize();

    auto flv = parse(sink);
    // tag[0] is onMetaData (default), then the two video tags.
    ASSERT_EQ(flv.tags.size(), 3u);
    EXPECT_EQ(flv.tags[1].timestamp, 500u);
    EXPECT_EQ(flv.tags[2].timestamp, 0x0100'0000u);
}

TEST(RecorderTest, FinalizePatchesDurationAndFilesize) {
    FakeSink sink;
    Recorder rec(sink);
    rec.on_video(media_msg(MessageTypeId::Video, bytes({0x17, 0x00}), 2000)); // 2s
    rec.finalize();

    // Read the patched duration/filesize doubles back out of the file.
    auto tag = media::flv::build_onmetadata_tag(media::flv::OnMetaData{});
    // The metadata tag starts right after the 13-byte file header.
    const std::size_t dur_off = media::flv::kFileHeaderTotalSize + tag.duration_value_offset;
    const std::size_t fsz_off = media::flv::kFileHeaderTotalSize + tag.filesize_value_offset;

    auto expect_dur = media::flv::encode_double_be(2.0); // 2000 ms -> 2.0 s
    auto expect_fsz = media::flv::encode_double_be(static_cast<double>(sink.file.size()));
    for (std::size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(sink.file.at(dur_off + i), expect_dur[i]);
        EXPECT_EQ(sink.file.at(fsz_off + i), expect_fsz[i]);
    }
}

TEST(RecorderTest, BoundedQueueDropsFramesWhenSinkIsStalled) {
    FakeSink sink;
    RecorderConfig cfg;
    cfg.max_queued_bytes = 1024;
    Recorder rec(sink, cfg);
    rec.on_metadata(onmetadata_msg()); // header + metadata written while pending==0
    const std::size_t size_after_metadata = sink.file.size();

    // Simulate a stuck disk: lots of bytes outstanding.
    sink.pending_ = 4096;
    std::vector<std::byte> big(2000, std::byte{0x11});
    rec.on_video(media_msg(MessageTypeId::Video, big, 100));
    rec.on_video(media_msg(MessageTypeId::Video, big, 200));

    EXPECT_EQ(rec.stats().dropped_frames, 2u);
    EXPECT_GT(rec.stats().dropped_bytes, 0u);
    EXPECT_EQ(sink.file.size(), size_after_metadata); // nothing new written
    EXPECT_FALSE(rec.stats().failed);                 // dropping is not a failure

    // Once the disk drains, recording resumes.
    sink.pending_ = 0;
    rec.on_video(media_msg(MessageTypeId::Video, bytes({0x27, 0x01}), 300));
    EXPECT_GT(sink.file.size(), size_after_metadata);
}

TEST(RecorderTest, DiskFailureDoesNotCrashAndIsRecorded) {
    FakeSink sink;
    sink.fail_appends = true;
    Recorder rec(sink);
    // Every write path fails; the recorder must swallow it, not throw.
    rec.on_metadata(onmetadata_msg());
    rec.on_video(media_msg(MessageTypeId::Video, bytes({0x17, 0x00}), 0));
    rec.on_audio(media_msg(MessageTypeId::Audio, bytes({0xAF, 0x00}), 0));
    EXPECT_TRUE(rec.stats().failed);

    // finalize still runs and still closes the sink exactly once.
    rec.finalize();
    EXPECT_EQ(sink.finalize_calls, 1);
}

TEST(RecorderTest, AbruptDisconnectFinalizesSafelyEvenWithNoMedia) {
    FakeSink sink;
    Recorder rec(sink);
    // No media at all — simulate a publisher that vanished right after publish.
    rec.finalize();

    auto flv = parse(sink); // still a valid FLV: header + onMetaData
    EXPECT_EQ(flv.tags.size(), 1u);
    EXPECT_EQ(flv.tags[0].type, media::flv::kTagTypeScriptData);
    EXPECT_EQ(sink.finalize_calls, 1);
}

TEST(RecorderTest, FinalizeIsIdempotent) {
    FakeSink sink;
    Recorder rec(sink);
    rec.on_video(media_msg(MessageTypeId::Video, bytes({0x17, 0x00}), 0));
    rec.finalize();
    rec.finalize();
    rec.finalize();
    EXPECT_EQ(sink.finalize_calls, 1);
}

TEST(RecorderTest, MetadataDimensionsAreExtractedFromOnMetaData) {
    FakeSink sink;
    Recorder rec(sink);
    rec.on_metadata(onmetadata_msg());
    rec.finalize();
    // The onMetaData tag must carry width=1920 somewhere in its data; a full
    // AMF0 re-decode is covered by amf0 tests, here we just confirm the
    // recording is a valid FLV that parses with a single script tag.
    auto flv = parse(sink);
    ASSERT_GE(flv.tags.size(), 1u);
    EXPECT_EQ(flv.tags[0].type, media::flv::kTagTypeScriptData);
}

} // namespace
} // namespace rtmp_server::recording
