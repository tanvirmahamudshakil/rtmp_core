#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace rtmp_server::protocol::chunk {
namespace {

// Hand-rolled byte-level builders mirroring the RTMP wire format exactly
// (docs/rtmp_promot.md "RTMP Chunk Protocol" / docs/chunk-parser.md) —
// deliberately independent of ChunkEncoder so decoder tests exercise the
// spec, not the encoder's interpretation of it.

void append_u24_be(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(v & 0xFF));
}

void append_u32_be(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(v & 0xFF));
}

void append_u32_le(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
}

void append_basic_header(std::vector<std::byte>& out, std::uint8_t fmt, std::uint32_t csid) {
    if (csid < 64) {
        out.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(fmt) << 6) | csid));
    } else if (csid < 64 + 256) {
        out.push_back(static_cast<std::byte>(fmt << 6));
        out.push_back(static_cast<std::byte>(csid - 64));
    } else {
        out.push_back(static_cast<std::byte>((fmt << 6) | 0x01));
        std::uint32_t rem = csid - 64;
        out.push_back(static_cast<std::byte>(rem & 0xFF));
        out.push_back(static_cast<std::byte>((rem >> 8) & 0xFF));
    }
}

std::vector<std::byte> payload_of(std::size_t n, std::byte fill = std::byte{0xAB}) {
    return std::vector<std::byte>(n, fill);
}

// fmt0: csid, ts, message_length, type, stream_id (LE), payload.
std::vector<std::byte> make_fmt0_chunk(std::uint32_t csid, std::uint32_t ts, std::uint8_t type,
                                        std::uint32_t stream_id, std::span<const std::byte> payload,
                                        std::uint32_t chunk_size = kDefaultChunkSize) {
    std::vector<std::byte> out;
    bool extended = ts >= kExtendedTimestampMarker;
    append_basic_header(out, 0, csid);
    append_u24_be(out, extended ? kExtendedTimestampMarker : ts);
    append_u24_be(out, static_cast<std::uint32_t>(payload.size()));
    out.push_back(static_cast<std::byte>(type));
    append_u32_le(out, stream_id);
    if (extended) append_u32_be(out, ts);
    std::size_t n = std::min<std::size_t>(chunk_size, payload.size());
    out.insert(out.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

std::vector<std::byte> make_fmt1_chunk(std::uint32_t csid, std::uint32_t delta, std::uint8_t type,
                                        std::span<const std::byte> payload,
                                        std::uint32_t chunk_size = kDefaultChunkSize) {
    std::vector<std::byte> out;
    bool extended = delta >= kExtendedTimestampMarker;
    append_basic_header(out, 1, csid);
    append_u24_be(out, extended ? kExtendedTimestampMarker : delta);
    append_u24_be(out, static_cast<std::uint32_t>(payload.size()));
    out.push_back(static_cast<std::byte>(type));
    if (extended) append_u32_be(out, delta);
    std::size_t n = std::min<std::size_t>(chunk_size, payload.size());
    out.insert(out.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

std::vector<std::byte> make_fmt2_chunk(std::uint32_t csid, std::uint32_t delta,
                                        std::span<const std::byte> payload,
                                        std::uint32_t chunk_size = kDefaultChunkSize) {
    std::vector<std::byte> out;
    bool extended = delta >= kExtendedTimestampMarker;
    append_basic_header(out, 2, csid);
    append_u24_be(out, extended ? kExtendedTimestampMarker : delta);
    if (extended) append_u32_be(out, delta);
    std::size_t n = std::min<std::size_t>(chunk_size, payload.size());
    out.insert(out.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

std::vector<std::byte> make_fmt3_chunk(std::uint32_t csid, std::span<const std::byte> payload,
                                        bool extended = false, std::uint32_t extended_value = 0,
                                        std::uint32_t chunk_size = kDefaultChunkSize) {
    std::vector<std::byte> out;
    append_basic_header(out, 3, csid);
    if (extended) append_u32_be(out, extended_value);
    std::size_t n = std::min<std::size_t>(chunk_size, payload.size());
    out.insert(out.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

class ChunkDecoderTest : public ::testing::Test {
protected:
    ChunkDecoderTest() : decoder_(kDefaultMax) {
        decoder_.set_message_handler([this](RtmpMessage msg) { messages_.push_back(std::move(msg)); });
        decoder_.set_error_handler([this](core::Error err) {
            failed_ = true;
            last_error_ = err;
        });
    }

    static constexpr std::uint32_t kDefaultMax = 1 << 20; // 1 MiB

    ChunkDecoder decoder_;
    std::vector<RtmpMessage> messages_;
    bool failed_ = false;
    core::Error last_error_;
};

TEST_F(ChunkDecoderTest, SingleFmt0ChunkProducesOneMessage) {
    auto payload = payload_of(20);
    auto chunk = make_fmt0_chunk(3, 1000, 8, 1, payload);

    decoder_.on_bytes_received(chunk);

    ASSERT_EQ(messages_.size(), 1u);
    EXPECT_EQ(messages_[0].chunk_stream_id, 3u);
    EXPECT_EQ(messages_[0].message_stream_id, 1u);
    EXPECT_EQ(messages_[0].message_type_id, 8u);
    EXPECT_EQ(messages_[0].timestamp, 1000u);
    ASSERT_EQ(messages_[0].payload.size(), payload.size());
    EXPECT_TRUE(std::equal(messages_[0].payload.begin(), messages_[0].payload.end(), payload.begin()));
    EXPECT_FALSE(failed_);
}

TEST_F(ChunkDecoderTest, TwoByteBasicHeaderFormParsesHighChunkStreamId) {
    auto payload = payload_of(10);
    auto chunk = make_fmt0_chunk(100, 0, 9, 5, payload); // csid 100 -> 2-byte basic header

    decoder_.on_bytes_received(chunk);

    ASSERT_EQ(messages_.size(), 1u);
    EXPECT_EQ(messages_[0].chunk_stream_id, 100u);
}

TEST_F(ChunkDecoderTest, ThreeByteBasicHeaderFormParsesVeryHighChunkStreamId) {
    auto payload = payload_of(10);
    auto chunk = make_fmt0_chunk(500, 0, 9, 5, payload); // csid 500 -> 3-byte basic header

    decoder_.on_bytes_received(chunk);

    ASSERT_EQ(messages_.size(), 1u);
    EXPECT_EQ(messages_[0].chunk_stream_id, 500u);
}

TEST_F(ChunkDecoderTest, Fmt1InheritsMessageStreamIdAndAppliesDelta) {
    auto payload0 = payload_of(15);
    decoder_.on_bytes_received(make_fmt0_chunk(4, 1000, 9, 7, payload0));
    ASSERT_EQ(messages_.size(), 1u);

    auto payload1 = payload_of(15, std::byte{0xCD});
    decoder_.on_bytes_received(make_fmt1_chunk(4, 40, 9, payload1)); // delta=40 -> ts=1040

    ASSERT_EQ(messages_.size(), 2u);
    EXPECT_EQ(messages_[1].message_stream_id, 7u); // inherited
    EXPECT_EQ(messages_[1].timestamp, 1040u);
    EXPECT_EQ(messages_[1].message_type_id, 9u);
}

TEST_F(ChunkDecoderTest, Fmt2InheritsLengthTypeAndStreamIdAppliesDelta) {
    auto payload0 = payload_of(12);
    decoder_.on_bytes_received(make_fmt0_chunk(5, 2000, 8, 3, payload0));
    ASSERT_EQ(messages_.size(), 1u);

    auto payload1 = payload_of(12, std::byte{0x11}); // same length as previous message
    decoder_.on_bytes_received(make_fmt2_chunk(5, 25, payload1));

    ASSERT_EQ(messages_.size(), 2u);
    EXPECT_EQ(messages_[1].message_stream_id, 3u);
    EXPECT_EQ(messages_[1].message_type_id, 8u);
    EXPECT_EQ(messages_[1].timestamp, 2025u);
    ASSERT_EQ(messages_[1].payload.size(), 12u);
}

TEST_F(ChunkDecoderTest, Fmt3InheritsEverythingIncludingDelta) {
    auto payload0 = payload_of(8);
    decoder_.on_bytes_received(make_fmt0_chunk(6, 500, 8, 2, payload0));
    auto payload1 = payload_of(8, std::byte{0x22});
    decoder_.on_bytes_received(make_fmt2_chunk(6, 30, payload1)); // ts=530, delta=30
    ASSERT_EQ(messages_.size(), 2u);

    auto payload2 = payload_of(8, std::byte{0x33});
    decoder_.on_bytes_received(make_fmt3_chunk(6, payload2)); // reuses delta=30 -> ts=560

    ASSERT_EQ(messages_.size(), 3u);
    EXPECT_EQ(messages_[2].message_stream_id, 2u);
    EXPECT_EQ(messages_[2].message_type_id, 8u);
    EXPECT_EQ(messages_[2].timestamp, 560u);
    ASSERT_EQ(messages_[2].payload.size(), 8u);
    EXPECT_EQ(messages_[2].payload[0], std::byte{0x33});
}

TEST_F(ChunkDecoderTest, PartialChunkOneByteAtATimeAssemblesCorrectly) {
    auto payload = payload_of(50);
    auto chunk = make_fmt0_chunk(3, 777, 9, 1, payload);

    for (auto b : chunk) {
        decoder_.on_bytes_received(std::span<const std::byte>(&b, 1));
    }

    ASSERT_EQ(messages_.size(), 1u);
    EXPECT_EQ(messages_[0].timestamp, 777u);
    ASSERT_EQ(messages_[0].payload.size(), 50u);
    EXPECT_TRUE(std::equal(messages_[0].payload.begin(), messages_[0].payload.end(), payload.begin()));
}

TEST_F(ChunkDecoderTest, PayloadLargerThanChunkSizeSplitsAcrossMultipleChunksAndReassembles) {
    // Default chunk size is 128; a 300-byte message requires 3 chunks: one
    // fmt0 (128 bytes) followed by two fmt3 continuations (128 + 44 bytes).
    std::vector<std::byte> payload(300);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i % 256);

    std::vector<std::byte> wire;
    auto first = make_fmt0_chunk(4, 10, 9, 1, payload, kDefaultChunkSize);
    wire.insert(wire.end(), first.begin(), first.end());
    auto second = make_fmt3_chunk(4, std::span(payload).subspan(128, 128));
    wire.insert(wire.end(), second.begin(), second.end());
    auto third = make_fmt3_chunk(4, std::span(payload).subspan(256, 44));
    wire.insert(wire.end(), third.begin(), third.end());

    // Feed in uneven fragments to also exercise partial delivery mid-payload.
    for (std::size_t offset = 0; offset < wire.size(); offset += 17) {
        std::size_t n = std::min<std::size_t>(17, wire.size() - offset);
        decoder_.on_bytes_received(std::span(wire).subspan(offset, n));
    }

    ASSERT_EQ(messages_.size(), 1u);
    ASSERT_EQ(messages_[0].payload.size(), 300u);
    EXPECT_TRUE(std::equal(messages_[0].payload.begin(), messages_[0].payload.end(), payload.begin()));
}

TEST_F(ChunkDecoderTest, InterleavedChunkStreamsDemuxIndependently) {
    auto payload_a1 = payload_of(10, std::byte{0xA1});
    auto payload_b1 = payload_of(20, std::byte{0xB1});
    auto a0 = make_fmt0_chunk(3, 100, 8, 1, payload_a1);
    auto b0 = make_fmt0_chunk(4, 200, 9, 2, payload_b1);

    // Interleave: start A (fmt0), start B (fmt0), continue A (fmt1), continue B (fmt2).
    decoder_.on_bytes_received(a0);
    decoder_.on_bytes_received(b0);
    auto payload_a2 = payload_of(10, std::byte{0xA2});
    decoder_.on_bytes_received(make_fmt1_chunk(3, 5, 8, payload_a2));
    auto payload_b2 = payload_of(20, std::byte{0xB2});
    decoder_.on_bytes_received(make_fmt2_chunk(4, 5, payload_b2));

    ASSERT_EQ(messages_.size(), 4u);
    EXPECT_EQ(messages_[0].chunk_stream_id, 3u);
    EXPECT_EQ(messages_[1].chunk_stream_id, 4u);
    EXPECT_EQ(messages_[2].chunk_stream_id, 3u);
    EXPECT_EQ(messages_[2].timestamp, 105u); // independent header state for csid 3
    EXPECT_EQ(messages_[3].chunk_stream_id, 4u);
    EXPECT_EQ(messages_[3].timestamp, 205u); // independent header state for csid 4
}

TEST_F(ChunkDecoderTest, ExtendedTimestampFmt0IsParsedCorrectly) {
    auto payload = payload_of(16);
    std::uint32_t big_ts = 0x01020304;
    auto chunk = make_fmt0_chunk(3, big_ts, 9, 1, payload);

    decoder_.on_bytes_received(chunk);

    ASSERT_EQ(messages_.size(), 1u);
    EXPECT_EQ(messages_[0].timestamp, big_ts);
}

TEST_F(ChunkDecoderTest, ExtendedTimestampFmt1DeltaIsParsedCorrectly) {
    auto payload0 = payload_of(16);
    decoder_.on_bytes_received(make_fmt0_chunk(3, 0, 9, 1, payload0));
    ASSERT_EQ(messages_.size(), 1u);

    std::uint32_t big_delta = 0x00FFFFFF + 12345;
    auto payload1 = payload_of(16, std::byte{0x99});
    decoder_.on_bytes_received(make_fmt1_chunk(3, big_delta, 9, payload1));

    ASSERT_EQ(messages_.size(), 2u);
    EXPECT_EQ(messages_[1].timestamp, big_delta);
}

TEST_F(ChunkDecoderTest, ExtendedTimestampFmt3RepeatsFieldAndContinuesDelta) {
    auto payload0 = payload_of(8);
    decoder_.on_bytes_received(make_fmt0_chunk(3, 0, 9, 1, payload0));
    std::uint32_t big_delta = kExtendedTimestampMarker + 500;
    auto payload1 = payload_of(8, std::byte{0x44});
    decoder_.on_bytes_received(make_fmt1_chunk(3, big_delta, 9, payload1)); // ts = big_delta
    ASSERT_EQ(messages_.size(), 2u);

    auto payload2 = payload_of(8, std::byte{0x55});
    // fmt3 after an extended-timestamp chunk must repeat the 4-byte field.
    decoder_.on_bytes_received(make_fmt3_chunk(3, payload2, /*extended=*/true, big_delta));

    ASSERT_EQ(messages_.size(), 3u);
    EXPECT_EQ(messages_[2].timestamp, big_delta + big_delta);
}

TEST_F(ChunkDecoderTest, ExtendedTimestampAssembledByteAtATimeAcrossExtendedField) {
    auto chunk = make_fmt0_chunk(3, 0x02000000, 9, 1, payload_of(5));
    for (auto b : chunk) decoder_.on_bytes_received(std::span<const std::byte>(&b, 1));

    ASSERT_EQ(messages_.size(), 1u);
    EXPECT_EQ(messages_[0].timestamp, 0x02000000u);
}

TEST_F(ChunkDecoderTest, OversizedMessageIsRejectedCleanly) {
    ChunkDecoder small_decoder(100); // max message size = 100 bytes
    bool small_failed = false;
    core::Error small_error;
    small_decoder.set_error_handler([&](core::Error err) {
        small_failed = true;
        small_error = err;
    });
    small_decoder.set_message_handler([](RtmpMessage) { FAIL() << "should not deliver an oversized message"; });

    auto payload = payload_of(500);
    auto chunk = make_fmt0_chunk(3, 0, 9, 1, payload, small_decoder.input_chunk_size());

    small_decoder.on_bytes_received(chunk);

    EXPECT_TRUE(small_failed);
    EXPECT_EQ(small_error.code(), core::ErrorCode::MessageTooLarge);
    EXPECT_TRUE(small_decoder.failed());
}

TEST_F(ChunkDecoderTest, NonFmt0FirstChunkOnNewStreamIsRejected) {
    auto payload = payload_of(10);
    auto chunk = make_fmt1_chunk(9, 5, 8, payload); // csid 9 never seen before

    decoder_.on_bytes_received(chunk);

    EXPECT_TRUE(failed_);
    EXPECT_EQ(last_error_.code(), core::ErrorCode::MalformedChunk);
    EXPECT_TRUE(messages_.empty());
}

TEST_F(ChunkDecoderTest, SetChunkSizeControlMessageUpdatesInputChunkSize) {
    std::vector<std::byte> scs;
    append_basic_header(scs, 0, kProtocolControlChunkStreamId);
    append_u24_be(scs, 0);
    append_u24_be(scs, 4);
    scs.push_back(static_cast<std::byte>(1)); // Set Chunk Size type id
    append_u32_le(scs, kProtocolControlMessageStreamId);
    append_u32_be(scs, 4096);

    decoder_.on_bytes_received(scs);

    EXPECT_EQ(decoder_.input_chunk_size(), 4096u);
    EXPECT_TRUE(messages_.empty()); // handled internally, not delivered to the app handler
    EXPECT_FALSE(failed_);

    // A subsequent message larger than the old 128-byte default but smaller
    // than the new 4096-byte size must now arrive as a single chunk.
    auto payload = payload_of(1000);
    decoder_.on_bytes_received(make_fmt0_chunk(3, 0, 9, 1, payload, decoder_.input_chunk_size()));
    ASSERT_EQ(messages_.size(), 1u);
    EXPECT_EQ(messages_[0].payload.size(), 1000u);
}

TEST_F(ChunkDecoderTest, WindowAcknowledgementSizeUpdatesAndAckIsDue) {
    std::vector<std::byte> was;
    append_basic_header(was, 0, kProtocolControlChunkStreamId);
    append_u24_be(was, 0);
    append_u24_be(was, 4);
    was.push_back(static_cast<std::byte>(5)); // Window Acknowledgement Size type id
    append_u32_le(was, kProtocolControlMessageStreamId);
    append_u32_be(was, 50);

    decoder_.on_bytes_received(was);
    EXPECT_EQ(decoder_.window_acknowledgement_size(), 50u);
    EXPECT_FALSE(decoder_.acknowledgement_due()); // 16 bytes received so far (< 50)

    auto payload = payload_of(60);
    decoder_.on_bytes_received(make_fmt0_chunk(3, 0, 9, 1, payload));
    EXPECT_TRUE(decoder_.acknowledgement_due());

    decoder_.mark_acknowledged();
    EXPECT_FALSE(decoder_.acknowledgement_due());
}

TEST_F(ChunkDecoderTest, AbortMessageDiscardsInProgressChunkData) {
    std::vector<std::byte> payload(300);
    auto first = make_fmt0_chunk(4, 0, 9, 1, payload, kDefaultChunkSize);
    decoder_.on_bytes_received(first); // only 128 of 300 bytes delivered so far; message in progress

    std::vector<std::byte> abort;
    append_basic_header(abort, 0, kProtocolControlChunkStreamId);
    append_u24_be(abort, 0);
    append_u24_be(abort, 4);
    abort.push_back(static_cast<std::byte>(2)); // Abort Message type id
    append_u32_le(abort, kProtocolControlMessageStreamId);
    append_u32_be(abort, 4); // target csid = 4

    decoder_.on_bytes_received(abort);

    // Restarting chunk stream 4 with a fresh fmt0 header must succeed cleanly
    // (no leftover partial state from the aborted message).
    auto payload2 = payload_of(10);
    decoder_.on_bytes_received(make_fmt0_chunk(4, 10, 8, 2, payload2));

    ASSERT_EQ(messages_.size(), 1u);
    EXPECT_EQ(messages_[0].payload.size(), 10u);
}

TEST_F(ChunkDecoderTest, MessagesAfterFailureAreIgnored) {
    decoder_.on_bytes_received(make_fmt1_chunk(9, 5, 8, payload_of(4))); // triggers MalformedChunk
    ASSERT_TRUE(failed_);
    failed_ = false;

    decoder_.on_bytes_received(make_fmt0_chunk(3, 0, 9, 1, payload_of(4)));

    EXPECT_TRUE(messages_.empty());
    EXPECT_FALSE(failed_);
}

} // namespace
} // namespace rtmp_server::protocol::chunk
