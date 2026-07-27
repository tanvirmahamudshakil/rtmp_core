// Phase 8 security tasks 5, 6, 8 and 9: regression tests for the input-size,
// recursion, allocation and timestamp-rollover limits.
//
// Every test here corresponds to a defect that was reproduced against the
// pre-Phase-8 code with a standalone proof-of-concept (see
// docs/phase-8-report.md "Problems confirmed"). They exist so those defects
// cannot silently return.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"

namespace {

using rtmp_server::core::ErrorCode;
namespace amf0 = rtmp_server::protocol::amf0;
namespace chunk = rtmp_server::protocol::chunk;

// ===========================================================================
// AMF0 recursion depth (task 5)
// ===========================================================================

// Builds `levels` nested AMF0 objects, each holding a single property "a"
// whose value is the next object down; the innermost value is null. This is
// the exact shape that segfaulted the pre-fix decoder.
std::vector<std::byte> nested_objects(std::size_t levels) {
    std::vector<std::byte> out;
    for (std::size_t i = 0; i < levels; ++i) {
        out.push_back(std::byte{0x03});             // object marker
        out.push_back(std::byte{0x00});             // property name length hi
        out.push_back(std::byte{0x01});             // property name length lo
        out.push_back(static_cast<std::byte>('a')); // property name
    }
    out.push_back(std::byte{0x05}); // innermost value: null
    // The nested objects are deliberately left unterminated: the decoder must
    // reject on depth before it ever reaches the missing terminators, which
    // is what proves the depth check (and not truncation) did the rejecting.
    return out;
}

TEST(AmfSecurityLimitsTest, AcceptsNestingUpToTheDocumentedDepth) {
    // A fully well-formed structure at a realistic depth must still decode,
    // so the limit does not break legitimate command objects.
    amf0::Amf0PropertyList inner;
    inner.emplace_back("x", amf0::Amf0Value::number(1));
    auto value = amf0::Amf0Value::object(std::move(inner));
    for (int i = 0; i < 8; ++i) {
        amf0::Amf0PropertyList wrap;
        wrap.emplace_back("a", std::move(value));
        value = amf0::Amf0Value::object(std::move(wrap));
    }
    std::vector<std::byte> encoded;
    amf0::encode(value, encoded);

    auto decoded = amf0::decode(encoded);
    EXPECT_TRUE(decoded.ok());
}

TEST(AmfSecurityLimitsTest, RejectsNestingBeyondTheMaximumDepth) {
    auto input = nested_objects(amf0::kMaxNestingDepth + 5);
    auto decoded = amf0::decode(input);
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), ErrorCode::MalformedAmf);
}

TEST(AmfSecurityLimitsTest, SurvivesTheDeepNestingPayloadThatUsedToSegfault) {
    // 200000 levels == the reproduced stack-overflow payload (800 KB). The
    // test passing at all is the assertion: pre-fix this crashed the process.
    auto input = nested_objects(200'000);
    auto decoded = amf0::decode(input);
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), ErrorCode::MalformedAmf);
}

TEST(AmfSecurityLimitsTest, DepthLimitAppliesToStrictArraysToo) {
    std::vector<std::byte> input;
    for (std::size_t i = 0; i < amf0::kMaxNestingDepth + 5; ++i) {
        input.push_back(std::byte{0x0A});                        // strict array marker
        input.push_back(std::byte{0x00});                        // count = 1
        input.push_back(std::byte{0x00});
        input.push_back(std::byte{0x00});
        input.push_back(std::byte{0x01});
    }
    input.push_back(std::byte{0x05});
    auto decoded = amf0::decode(input);
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), ErrorCode::MalformedAmf);
}

TEST(AmfSecurityLimitsTest, DecodeAllResetsDepthBetweenTopLevelValues) {
    // A command payload is several top-level values in a row; each must get
    // the full depth budget rather than sharing one running counter.
    std::vector<std::byte> input;
    for (int v = 0; v < 4; ++v) {
        amf0::Amf0Value value = amf0::Amf0Value::number(1);
        for (int i = 0; i < 10; ++i) {
            amf0::Amf0PropertyList wrap;
            wrap.emplace_back("a", std::move(value));
            value = amf0::Amf0Value::object(std::move(wrap));
        }
        amf0::encode(value, input);
    }
    auto decoded = amf0::decode_all(input);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().size(), 4u);
}

// ===========================================================================
// AMF0 client-controlled allocation (task 6)
// ===========================================================================

TEST(AmfSecurityLimitsTest, RejectsStrictArrayCountLargerThanTheBytesRemaining) {
    // 5 bytes claiming 4294967295 elements. Pre-fix this reached
    // vector::reserve() and requested ~275 GB of address space.
    const std::vector<std::byte> input{std::byte{0x0A}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                                        std::byte{0xFF}};
    auto decoded = amf0::decode(input);
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), ErrorCode::MalformedAmf);
}

TEST(AmfSecurityLimitsTest, RejectsStrictArrayCountJustBeyondTheAvailableBytes) {
    // count = 100 with only a handful of payload bytes: each element needs at
    // least one byte, so this is provably a lie without decoding anything.
    std::vector<std::byte> input{std::byte{0x0A}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x64}};
    for (int i = 0; i < 10; ++i) input.push_back(std::byte{0x05});
    auto decoded = amf0::decode(input);
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code(), ErrorCode::MalformedAmf);
}

TEST(AmfSecurityLimitsTest, StillDecodesAHonestlySizedStrictArray) {
    std::vector<amf0::Amf0Value> items;
    for (int i = 0; i < 16; ++i) items.push_back(amf0::Amf0Value::number(i));
    std::vector<std::byte> encoded;
    amf0::encode(amf0::Amf0Value::strict_array(std::move(items)), encoded);

    auto decoded = amf0::decode(encoded);
    ASSERT_TRUE(decoded.ok());
    ASSERT_TRUE(decoded.value().value.is_strict_array());
    EXPECT_EQ(decoded.value().value.as_strict_array().size(), 16u);
}

// ===========================================================================
// Chunk reassembly bounds (tasks 5 and 6)
// ===========================================================================

// Emits one fmt-0 chunk on an extended (3-byte basic header) chunk stream ID
// declaring `declared_length` bytes of payload, followed by `payload_bytes`
// actual payload bytes.
void append_fmt0_chunk(std::vector<std::byte>& out, std::uint32_t csid, std::uint32_t declared_length,
                       std::size_t payload_bytes) {
    const std::uint32_t offset = csid - 64;
    out.push_back(std::byte{0x01}); // fmt 0, csid field 1 => 3-byte basic header
    out.push_back(static_cast<std::byte>(offset & 0xFF));
    out.push_back(static_cast<std::byte>((offset >> 8) & 0xFF));
    out.push_back(std::byte{0}); // timestamp (24-bit)
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    out.push_back(static_cast<std::byte>((declared_length >> 16) & 0xFF)); // message length (24-bit)
    out.push_back(static_cast<std::byte>((declared_length >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(declared_length & 0xFF));
    out.push_back(std::byte{0x09}); // type: video
    for (int i = 0; i < 4; ++i) out.push_back(std::byte{0}); // message stream id
    for (std::size_t i = 0; i < payload_bytes; ++i) out.push_back(std::byte{0x42});
}

struct DecoderHarness {
    chunk::ChunkDecoder decoder;
    std::vector<rtmp_server::core::Error> errors;
    std::size_t messages = 0;

    explicit DecoderHarness(std::uint32_t max_message_size = 10 * 1024 * 1024, chunk::ChunkDecoderLimits limits = {})
        : decoder(max_message_size, limits) {
        decoder.set_message_handler([this](chunk::RtmpMessage) { ++messages; });
        decoder.set_error_handler([this](rtmp_server::core::Error e) { errors.push_back(std::move(e)); });
    }
};

TEST(ChunkSecurityLimitsTest, RejectsMoreConcurrentChunkStreamsThanTheLimit) {
    DecoderHarness h;
    const auto limit = h.decoder.limits().max_chunk_streams;

    std::vector<std::byte> input;
    for (std::uint32_t i = 0; i < limit + 10; ++i) {
        // Declare 10 MiB but send exactly one default-size chunk of payload,
        // so every chunk stream stays open — the hoarding pattern.
        append_fmt0_chunk(input, 64 + i, 10u * 1024u * 1024u, chunk::kDefaultChunkSize);
    }
    h.decoder.on_bytes_received(input);

    EXPECT_TRUE(h.decoder.failed());
    ASSERT_FALSE(h.errors.empty());
    EXPECT_EQ(h.errors.front().code(), ErrorCode::MalformedChunk);
}

TEST(ChunkSecurityLimitsTest, MemoryStaysProportionalToBytesActuallySent) {
    // The pre-fix decoder reserved the full declared length per chunk stream,
    // so 63 open streams committed ~630 MiB. Now the outstanding payload must
    // track the bytes really delivered.
    DecoderHarness h;
    const auto limit = h.decoder.limits().max_chunk_streams;

    std::vector<std::byte> input;
    const std::size_t per_stream = chunk::kDefaultChunkSize;
    for (std::uint32_t i = 0; i < limit - 1; ++i) {
        append_fmt0_chunk(input, 64 + i, 10u * 1024u * 1024u, per_stream);
    }
    h.decoder.on_bytes_received(input);

    ASSERT_FALSE(h.decoder.failed());
    EXPECT_EQ(h.decoder.chunk_stream_count(), limit - 1);
    EXPECT_EQ(h.decoder.buffered_payload_bytes(), (limit - 1) * per_stream);
}

TEST(ChunkSecurityLimitsTest, RejectsWhenOutstandingReassemblyBytesExceedTheLimit) {
    chunk::ChunkDecoderLimits limits;
    limits.max_buffered_payload_bytes = 4 * chunk::kDefaultChunkSize;
    DecoderHarness h(10 * 1024 * 1024, limits);

    std::vector<std::byte> input;
    for (std::uint32_t i = 0; i < 10; ++i) {
        append_fmt0_chunk(input, 64 + i, 10u * 1024u * 1024u, chunk::kDefaultChunkSize);
    }
    h.decoder.on_bytes_received(input);

    EXPECT_TRUE(h.decoder.failed());
    ASSERT_FALSE(h.errors.empty());
    EXPECT_EQ(h.errors.front().code(), ErrorCode::MessageTooLarge);
}

TEST(ChunkSecurityLimitsTest, CompletedMessagesReleaseTheirReassemblyBudget) {
    DecoderHarness h;
    std::vector<std::byte> input;
    // Many complete, small messages on one chunk stream must not accumulate.
    for (int i = 0; i < 500; ++i) append_fmt0_chunk(input, 64, 4, 4);
    h.decoder.on_bytes_received(input);

    ASSERT_FALSE(h.decoder.failed());
    EXPECT_EQ(h.messages, 500u);
    EXPECT_EQ(h.decoder.buffered_payload_bytes(), 0u);
}

TEST(ChunkSecurityLimitsTest, AbortMessageReleasesTheReassemblyBudget) {
    DecoderHarness h;
    std::vector<std::byte> input;
    append_fmt0_chunk(input, 64, 10u * 1024u * 1024u, chunk::kDefaultChunkSize);
    h.decoder.on_bytes_received(input);
    ASSERT_EQ(h.decoder.buffered_payload_bytes(), chunk::kDefaultChunkSize);

    // Abort Message (type 2) on the protocol-control chunk stream, targeting
    // csid 64.
    std::vector<std::byte> abort;
    abort.push_back(std::byte{0x02}); // fmt 0, csid 2
    for (int i = 0; i < 3; ++i) abort.push_back(std::byte{0});
    abort.push_back(std::byte{0});
    abort.push_back(std::byte{0});
    abort.push_back(std::byte{4});    // length 4
    abort.push_back(std::byte{0x02}); // type: Abort Message
    for (int i = 0; i < 4; ++i) abort.push_back(std::byte{0});
    abort.push_back(std::byte{0});
    abort.push_back(std::byte{0});
    abort.push_back(std::byte{0});
    abort.push_back(std::byte{64}); // target csid 64
    h.decoder.on_bytes_received(abort);

    ASSERT_FALSE(h.decoder.failed());
    EXPECT_EQ(h.decoder.buffered_payload_bytes(), 0u);
}

TEST(ChunkSecurityLimitsTest, RejectsSetChunkSizeAboveTheSpecificationMaximum) {
    DecoderHarness h;
    std::vector<std::byte> input;
    input.push_back(std::byte{0x02});
    for (int i = 0; i < 3; ++i) input.push_back(std::byte{0});
    input.push_back(std::byte{0});
    input.push_back(std::byte{0});
    input.push_back(std::byte{4});
    input.push_back(std::byte{0x01}); // Set Chunk Size
    for (int i = 0; i < 4; ++i) input.push_back(std::byte{0});
    input.push_back(std::byte{0x7F}); // 0x7FFFFFFF after the high-bit mask
    input.push_back(std::byte{0xFF});
    input.push_back(std::byte{0xFF});
    input.push_back(std::byte{0xFF});
    h.decoder.on_bytes_received(input);

    EXPECT_TRUE(h.decoder.failed());
    ASSERT_FALSE(h.errors.empty());
    EXPECT_EQ(h.errors.front().code(), ErrorCode::MalformedChunk);
}

TEST(ChunkSecurityLimitsTest, AcceptsSetChunkSizeAtTheSpecificationMaximum) {
    DecoderHarness h;
    std::vector<std::byte> input;
    input.push_back(std::byte{0x02});
    for (int i = 0; i < 3; ++i) input.push_back(std::byte{0});
    input.push_back(std::byte{0});
    input.push_back(std::byte{0});
    input.push_back(std::byte{4});
    input.push_back(std::byte{0x01});
    for (int i = 0; i < 4; ++i) input.push_back(std::byte{0});
    input.push_back(std::byte{0x00}); // 0x00FFFFFF
    input.push_back(std::byte{0xFF});
    input.push_back(std::byte{0xFF});
    input.push_back(std::byte{0xFF});
    h.decoder.on_bytes_received(input);

    EXPECT_FALSE(h.decoder.failed());
    EXPECT_EQ(h.decoder.input_chunk_size(), 0xFFFFFFu);
}

TEST(ChunkSecurityLimitsTest, RejectsDeclaredMessageLengthAboveTheMaximum) {
    DecoderHarness h(1024);
    std::vector<std::byte> input;
    append_fmt0_chunk(input, 64, 0xFFFFFF, 0);
    h.decoder.on_bytes_received(input);

    EXPECT_TRUE(h.decoder.failed());
    ASSERT_FALSE(h.errors.empty());
    EXPECT_EQ(h.errors.front().code(), ErrorCode::MessageTooLarge);
}

// ===========================================================================
// Timestamp rollover (task 9)
// ===========================================================================

TEST(ChunkSecurityLimitsTest, TimestampWrapsModulo32BitsRatherThanSaturating) {
    // RTMP timestamps are unsigned 32-bit milliseconds and are specified to
    // wrap (~49.7 days of continuous stream). The decoder must reproduce the
    // wrap exactly: base + delta computed in uint32 arithmetic. Verified here
    // via the extended-timestamp path, which is the only way to express a
    // base near 2^32.
    DecoderHarness h;
    std::vector<std::byte> input;

    // fmt 0 with the 0xFFFFFF escape => 4-byte extended timestamp follows the
    // message header, carrying an absolute base of 0xFFFFFFF0.
    input.push_back(std::byte{0x03}); // fmt 0, csid 3
    input.push_back(std::byte{0xFF}); // timestamp escape
    input.push_back(std::byte{0xFF});
    input.push_back(std::byte{0xFF});
    input.push_back(std::byte{0}); // length 4
    input.push_back(std::byte{0});
    input.push_back(std::byte{4});
    input.push_back(std::byte{0x09});
    for (int i = 0; i < 4; ++i) input.push_back(std::byte{0});
    input.push_back(std::byte{0xFF}); // extended timestamp 0xFFFFFFF0
    input.push_back(std::byte{0xFF});
    input.push_back(std::byte{0xFF});
    input.push_back(std::byte{0xF0});
    for (int i = 0; i < 4; ++i) input.push_back(std::byte{0x11});

    std::vector<std::uint32_t> timestamps;
    h.decoder.set_message_handler([&](chunk::RtmpMessage m) { timestamps.push_back(m.timestamp); });
    h.decoder.on_bytes_received(input);

    // fmt 2: 3-byte delta of 0x20, which must wrap 0xFFFFFFF0 to 0x10.
    std::vector<std::byte> next;
    next.push_back(std::byte{0x83}); // fmt 2, csid 3
    next.push_back(std::byte{0x00});
    next.push_back(std::byte{0x00});
    next.push_back(std::byte{0x20});
    for (int i = 0; i < 4; ++i) next.push_back(std::byte{0x22});
    h.decoder.on_bytes_received(next);

    ASSERT_FALSE(h.decoder.failed());
    ASSERT_EQ(timestamps.size(), 2u);
    EXPECT_EQ(timestamps[0], 0xFFFFFFF0u);
    EXPECT_EQ(timestamps[1], 0x10u) << "32-bit timestamp must wrap, not saturate or overflow into UB";
}

} // namespace
