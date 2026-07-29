#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"

namespace rtmp_server::protocol::chunk {
namespace {

std::vector<std::byte> payload_of(std::size_t n, std::byte fill = std::byte{0x7A}) {
    return std::vector<std::byte>(n, fill);
}

RtmpMessage make_message(std::uint32_t csid, std::uint32_t stream_id, std::uint8_t type,
                          std::uint32_t timestamp, std::vector<std::byte> payload) {
    RtmpMessage m;
    m.chunk_stream_id = csid;
    m.message_stream_id = stream_id;
    m.message_type_id = type;
    m.timestamp = timestamp;
    m.payload = std::move(payload);
    return m;
}

TEST(ChunkEncoderTest, FirstMessageOnAChunkStreamUsesFmt0) {
    ChunkEncoder encoder;
    std::vector<std::byte> out;
    auto payload = payload_of(10);

    encoder.encode_message(make_message(3, 1, 8, 1000, payload), out);

    ASSERT_GE(out.size(), 12u);
    std::uint8_t first_byte = static_cast<std::uint8_t>(out[0]);
    EXPECT_EQ((first_byte >> 6) & 0x03, 0); // fmt 0
    EXPECT_EQ(first_byte & 0x3F, 3u);       // csid 3, 1-byte basic header
    // ts (3 bytes BE)
    EXPECT_EQ(static_cast<std::uint8_t>(out[1]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[2]), 0x03);
    EXPECT_EQ(static_cast<std::uint8_t>(out[3]), 0xE8); // 1000 = 0x0003E8
    // message length (3 bytes BE) = 10
    EXPECT_EQ(static_cast<std::uint8_t>(out[4]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[5]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[6]), 0x0A);
    // type id
    EXPECT_EQ(static_cast<std::uint8_t>(out[7]), 8u);
    // stream id (4 bytes LE) = 1
    EXPECT_EQ(static_cast<std::uint8_t>(out[8]), 0x01);
    EXPECT_EQ(static_cast<std::uint8_t>(out[9]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[10]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[11]), 0x00);
    // payload follows
    ASSERT_EQ(out.size(), 12u + 10u);
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), out.begin() + 12));
}

TEST(ChunkEncoderTest, SecondMessageSameHeaderShapeUsesFmt3) {
    ChunkEncoder encoder;
    std::vector<std::byte> out;
    encoder.encode_message(make_message(3, 1, 8, 1000, payload_of(10)), out);
    out.clear();

    // Same stream id, same length/type, same delta as before (10 -> next
    // call establishes delta baseline of 0 since first message is fmt0).
    // Send a second message whose delta equals the first delta (0) is a
    // degenerate case; instead send two deltas to hit fmt2 then fmt3.
    encoder.encode_message(make_message(3, 1, 8, 1050, payload_of(10)), out); // delta=50 -> fmt2 (len/type unchanged)
    ASSERT_FALSE(out.empty());
    EXPECT_EQ((static_cast<std::uint8_t>(out[0]) >> 6) & 0x03, 2); // fmt2
    out.clear();

    encoder.encode_message(make_message(3, 1, 8, 1100, payload_of(10)), out); // delta=50 again -> fmt3
    ASSERT_FALSE(out.empty());
    EXPECT_EQ((static_cast<std::uint8_t>(out[0]) >> 6) & 0x03, 3); // fmt3
    EXPECT_EQ(out.size(), 1u + 10u);                               // basic header only + payload
}

TEST(ChunkEncoderTest, ChangedLengthOrTypeUsesFmt1) {
    ChunkEncoder encoder;
    std::vector<std::byte> out;
    encoder.encode_message(make_message(3, 1, 8, 1000, payload_of(10)), out);
    out.clear();

    encoder.encode_message(make_message(3, 1, 9, 1010, payload_of(20)), out); // type + length changed
    ASSERT_FALSE(out.empty());
    EXPECT_EQ((static_cast<std::uint8_t>(out[0]) >> 6) & 0x03, 1); // fmt1
    EXPECT_EQ(out.size(), 8u + 20u);                               // 1-byte basic + 7-byte header + payload
}

TEST(ChunkEncoderTest, ChangedMessageStreamIdForcesFmt0) {
    ChunkEncoder encoder;
    std::vector<std::byte> out;
    encoder.encode_message(make_message(3, 1, 8, 1000, payload_of(10)), out);
    out.clear();

    encoder.encode_message(make_message(3, 2, 8, 1010, payload_of(10)), out); // stream id changed
    ASSERT_FALSE(out.empty());
    EXPECT_EQ((static_cast<std::uint8_t>(out[0]) >> 6) & 0x03, 0); // fmt0
}

TEST(ChunkEncoderTest, TwoByteAndThreeByteBasicHeaderFormsAreChosenByChunkStreamId) {
    ChunkEncoder encoder;
    std::vector<std::byte> out;

    encoder.encode_message(make_message(100, 1, 8, 0, payload_of(1)), out);
    EXPECT_EQ(static_cast<std::uint8_t>(out[0]) & 0x3F, 0u); // csid field 0 -> 2-byte form
    EXPECT_EQ(static_cast<std::uint8_t>(out[1]), 100 - 64);
    out.clear();

    encoder.encode_message(make_message(500, 1, 8, 0, payload_of(1)), out);
    EXPECT_EQ(static_cast<std::uint8_t>(out[0]) & 0x3F, 1u); // csid field 1 -> 3-byte form
    std::uint32_t rem = static_cast<std::uint8_t>(out[1]) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(out[2])) << 8);
    EXPECT_EQ(rem, 500u - 64u);
}

TEST(ChunkEncoderTest, PayloadLargerThanChunkSizeSplitsIntoFmt3Continuations) {
    ChunkEncoder encoder(128);
    std::vector<std::byte> out;
    auto payload = payload_of(300, std::byte{0x5C});

    encoder.encode_message(make_message(3, 1, 9, 10, payload), out);

    // fmt0 header (12 bytes) + 128 payload, fmt3 (1 byte) + 128 payload, fmt3 (1 byte) + 44 payload.
    ASSERT_EQ(out.size(), 12u + 128u + 1u + 128u + 1u + 44u);
    EXPECT_EQ(static_cast<std::uint8_t>(out[12 + 128]) >> 6 & 0x03, 3);
    EXPECT_EQ(static_cast<std::uint8_t>(out[12 + 128 + 1 + 128]) >> 6 & 0x03, 3);
}

TEST(ChunkEncoderTest, ExtendedTimestampIsEmittedWhenTimestampExceedsThreshold) {
    ChunkEncoder encoder;
    std::vector<std::byte> out;
    std::uint32_t big_ts = 0x01020304;

    encoder.encode_message(make_message(3, 1, 8, big_ts, payload_of(5)), out);

    // ts field must read as the 0xFFFFFF marker, followed by the real 4-byte value.
    EXPECT_EQ(static_cast<std::uint8_t>(out[1]), 0xFF);
    EXPECT_EQ(static_cast<std::uint8_t>(out[2]), 0xFF);
    EXPECT_EQ(static_cast<std::uint8_t>(out[3]), 0xFF);
    std::uint32_t extended = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(out[12])) << 24) |
                              (static_cast<std::uint32_t>(static_cast<std::uint8_t>(out[13])) << 16) |
                              (static_cast<std::uint32_t>(static_cast<std::uint8_t>(out[14])) << 8) |
                              static_cast<std::uint32_t>(static_cast<std::uint8_t>(out[15]));
    EXPECT_EQ(extended, big_ts);
}

TEST(ChunkEncoderTest, SetChunkSizeHelperEncodesProtocolControlMessage) {
    ChunkEncoder encoder;
    std::vector<std::byte> out;
    encoder.encode_set_chunk_size(4096, out);

    ChunkDecoder decoder(1 << 20);
    RtmpMessage received;
    bool got = false;
    decoder.set_message_handler([&](RtmpMessage m) {
        received = std::move(m);
        got = true;
    });
    decoder.on_bytes_received(out);

    // Set Chunk Size is handled internally by the decoder (not delivered),
    // but its effect (updated input_chunk_size) is directly observable.
    EXPECT_FALSE(got);
    EXPECT_EQ(decoder.input_chunk_size(), 4096u);
}

TEST(ChunkEncoderTest, WindowAcknowledgementSizeAndSetPeerBandwidthRoundTripThroughDecoder) {
    ChunkEncoder encoder;
    std::vector<std::byte> out;
    encoder.encode_window_acknowledgement_size(2'500'000, out);
    encoder.encode_set_peer_bandwidth(2'500'000, PeerBandwidthLimitType::Dynamic, out);
    encoder.encode_acknowledgement(123456, out);
    encoder.encode_abort_message(7, out);

    ChunkDecoder decoder(1 << 20);
    std::vector<RtmpMessage> received;
    decoder.set_message_handler([&](RtmpMessage m) { received.push_back(std::move(m)); });
    decoder.on_bytes_received(out);

    EXPECT_EQ(decoder.window_acknowledgement_size(), 2'500'000u);
    // Window Ack Size is delivered *and* applied; Set Peer Bandwidth and
    // Acknowledgement are delivered as opaque messages; Abort Message is
    // handled internally only.
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0].message_type_id, static_cast<std::uint8_t>(MessageTypeId::WindowAcknowledgementSize));
    EXPECT_EQ(received[1].message_type_id, static_cast<std::uint8_t>(MessageTypeId::SetPeerBandwidth));
    EXPECT_EQ(received[1].payload[4], std::byte{static_cast<std::uint8_t>(PeerBandwidthLimitType::Dynamic)});
    EXPECT_EQ(received[2].message_type_id, static_cast<std::uint8_t>(MessageTypeId::Acknowledgement));
}

TEST(ChunkEncoderTest, RoundTripThroughDecoderPreservesArbitraryMessageSequence) {
    ChunkEncoder encoder(64); // small chunk size to force lots of splitting
    std::vector<std::byte> out;
    // The decoder has no independent knowledge of the encoder's chunk size —
    // on the wire this is communicated via a Set Chunk Size protocol-control
    // message, exactly like a real peer would send.
    encoder.encode_set_chunk_size(64, out);

    std::vector<RtmpMessage> originals;
    originals.push_back(make_message(3, 1, 8, 0, payload_of(10, std::byte{0x01})));
    originals.push_back(make_message(4, 2, 9, 0, payload_of(200, std::byte{0x02})));
    originals.push_back(make_message(3, 1, 8, 40, payload_of(10, std::byte{0x03})));
    originals.push_back(make_message(4, 2, 9, 33, payload_of(200, std::byte{0x04})));
    originals.push_back(make_message(3, 1, 8, 40, payload_of(10, std::byte{0x05}))); // same delta -> fmt3

    for (const auto& m : originals) encoder.encode_message(m, out);

    ChunkDecoder decoder(1 << 20);
    std::vector<RtmpMessage> received;
    decoder.set_message_handler([&](RtmpMessage m) { received.push_back(std::move(m)); });

    // Feed byte-at-a-time to also exercise partial-chunk reassembly on the
    // decode side of the round trip.
    for (auto b : out) decoder.on_bytes_received(std::span<const std::byte>(&b, 1));

    ASSERT_EQ(received.size(), originals.size());
    for (std::size_t i = 0; i < originals.size(); ++i) {
        EXPECT_EQ(received[i].chunk_stream_id, originals[i].chunk_stream_id);
        EXPECT_EQ(received[i].message_stream_id, originals[i].message_stream_id);
        EXPECT_EQ(received[i].message_type_id, originals[i].message_type_id);
        EXPECT_EQ(received[i].timestamp, originals[i].timestamp);
        ASSERT_EQ(received[i].payload.size(), originals[i].payload.size());
        EXPECT_TRUE(std::equal(received[i].payload.begin(), received[i].payload.end(), originals[i].payload.begin()));
    }
}

TEST(ChunkEncoderTest, StatelessFmt0EncodingRoundTripsAndDoesNotNeedPriorHeaderState) {
    constexpr std::uint32_t kChunkSize = 64;
    const auto original =
        make_message(5, 7, static_cast<std::uint8_t>(MessageTypeId::Video),
                     0x01020304, payload_of(257, std::byte{0x5A}));

    std::vector<std::byte> out;
    // Tell a fresh peer about the non-default chunk size first, exactly as
    // RtmpConnectionSession::start() does.
    ChunkEncoder setup;
    setup.encode_set_chunk_size(kChunkSize, out);
    ChunkEncoder::encode_message_fmt0(
        kChunkSize, original.chunk_stream_id, original.message_stream_id,
        original.message_type_id, original.timestamp, original.payload, out);

    ChunkDecoder decoder(1 << 20);
    std::vector<RtmpMessage> received;
    decoder.set_message_handler([&](RtmpMessage message) {
        received.push_back(std::move(message));
    });
    decoder.on_bytes_received(out);

    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].chunk_stream_id, original.chunk_stream_id);
    EXPECT_EQ(received[0].message_stream_id, original.message_stream_id);
    EXPECT_EQ(received[0].message_type_id, original.message_type_id);
    EXPECT_EQ(received[0].timestamp, original.timestamp);
    EXPECT_EQ(received[0].payload, original.payload);
}

} // namespace
} // namespace rtmp_server::protocol::chunk
