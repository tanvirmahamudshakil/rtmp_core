#include <gtest/gtest.h>

#include <map>
#include <vector>

#include "rtmp_server/media/ts/ts_muxer.hpp"

using namespace rtmp_server;
using namespace rtmp_server::media::ts;

namespace {

std::span<const std::byte> span_of(const std::vector<std::byte>& v) {
    return std::span<const std::byte>(v.data(), v.size());
}

unsigned u8(const std::vector<std::byte>& d, std::size_t i) {
    return static_cast<unsigned>(static_cast<std::uint8_t>(d[i]));
}

struct TsPacket {
    unsigned pid = 0;
    bool payload_unit_start = false;
    unsigned continuity_counter = 0;
    bool has_adaptation = false;
    bool has_payload = false;
    bool random_access = false;
    bool has_pcr = false;
    std::size_t offset = 0;
};

// A deliberately strict TS reader: it re-derives the framing from the bytes
// rather than trusting the muxer, so it can act as the conformance check
// ffprobe would otherwise provide (ffmpeg/ffprobe are not installed on this
// host — see docs/phase-6-report.md).
std::vector<TsPacket> parse_ts(const std::vector<std::byte>& data) {
    std::vector<TsPacket> packets;
    for (std::size_t offset = 0; offset + kPacketSize <= data.size(); offset += kPacketSize) {
        TsPacket p;
        p.offset = offset;
        EXPECT_EQ(u8(data, offset), kSyncByte) << "lost sync at offset " << offset;
        p.payload_unit_start = (u8(data, offset + 1) & 0x40) != 0;
        p.pid = ((u8(data, offset + 1) & 0x1F) << 8) | u8(data, offset + 2);
        const unsigned afc = (u8(data, offset + 3) >> 4) & 0x03;
        p.has_adaptation = (afc & 0x02) != 0;
        p.has_payload = (afc & 0x01) != 0;
        p.continuity_counter = u8(data, offset + 3) & 0x0F;
        if (p.has_adaptation) {
            const unsigned af_len = u8(data, offset + 4);
            if (af_len > 0) {
                const unsigned flags = u8(data, offset + 5);
                p.random_access = (flags & 0x40) != 0;
                p.has_pcr = (flags & 0x10) != 0;
            }
        }
        packets.push_back(p);
    }
    return packets;
}

std::vector<std::byte> annexb_frame(std::size_t size, unsigned nal_type) {
    std::vector<std::byte> out{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
    out.push_back(static_cast<std::byte>(nal_type));
    for (std::size_t i = 0; i < size; ++i) out.push_back(std::byte{0x55});
    return out;
}

} // namespace

TEST(TsMuxerTest, Crc32MatchesTheKnownMpeg2SystemsVector) {
    // "123456789" under MPEG-2 systems CRC-32 is the standard check value.
    const std::string text = "123456789";
    std::vector<std::byte> data;
    for (char c : text) data.push_back(static_cast<std::byte>(c));
    EXPECT_EQ(mpeg_crc32(span_of(data)), 0x0376E6E7u);
}

TEST(TsMuxerTest, ProgramTablesAreTwoWellFormedPackets) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    muxer.write_program_tables(out);

    ASSERT_EQ(out.size(), 2 * kPacketSize);
    auto packets = parse_ts(out);
    ASSERT_EQ(packets.size(), 2u);

    // PAT on PID 0, PMT on the configured PID, both starting a section.
    EXPECT_EQ(packets[0].pid, 0x0000u);
    EXPECT_TRUE(packets[0].payload_unit_start);
    EXPECT_EQ(packets[1].pid, muxer.config().pmt_pid);
    EXPECT_TRUE(packets[1].payload_unit_start);
}

TEST(TsMuxerTest, PsiSectionCrcsAreValid) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    muxer.write_program_tables(out);

    // For each PSI packet: skip the 4-byte TS header and the pointer field,
    // read section_length, then verify the trailing CRC over the section.
    for (std::size_t base : {std::size_t{0}, kPacketSize}) {
        const std::size_t section_start = base + 5; // 4 header + 1 pointer_field
        const unsigned section_length =
            ((u8(out, section_start + 1) & 0x0F) << 8) | u8(out, section_start + 2);
        // section_length counts bytes after the length field itself.
        const std::size_t total = 3 + section_length;
        std::vector<std::byte> section(out.begin() + static_cast<std::ptrdiff_t>(section_start),
                                       out.begin() + static_cast<std::ptrdiff_t>(section_start + total));
        ASSERT_GE(section.size(), 4u);

        std::vector<std::byte> body(section.begin(), section.end() - 4);
        const std::uint32_t expected = mpeg_crc32(span_of(body));
        const std::uint32_t actual = (static_cast<std::uint32_t>(u8(section, section.size() - 4)) << 24) |
                                     (static_cast<std::uint32_t>(u8(section, section.size() - 3)) << 16) |
                                     (static_cast<std::uint32_t>(u8(section, section.size() - 2)) << 8) |
                                     static_cast<std::uint32_t>(u8(section, section.size() - 1));
        EXPECT_EQ(actual, expected) << "bad CRC in section at " << base;
    }
}

TEST(TsMuxerTest, PmtDeclaresH264AndAacStreamTypes) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    muxer.write_program_tables(out);

    // PMT section: header is 12 bytes before the first ES entry
    // (table_id..program_info_length), starting after the pointer field.
    const std::size_t es = kPacketSize + 5 + 12;
    EXPECT_EQ(u8(out, es), 0x1Bu); // H.264
    const unsigned video_pid = ((u8(out, es + 1) & 0x1F) << 8) | u8(out, es + 2);
    EXPECT_EQ(video_pid, muxer.config().video_pid);

    EXPECT_EQ(u8(out, es + 5), 0x0Fu); // AAC ADTS
    const unsigned audio_pid = ((u8(out, es + 6) & 0x1F) << 8) | u8(out, es + 7);
    EXPECT_EQ(audio_pid, muxer.config().audio_pid);
}

TEST(TsMuxerTest, EveryEmittedPacketIsExactly188Bytes) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    muxer.write_program_tables(out);
    ASSERT_TRUE(muxer.write_video(out, span_of(annexb_frame(5000, 0x65)), 90000, 90000, true).ok());
    ASSERT_TRUE(muxer.write_audio(out, span_of(annexb_frame(300, 0x00)), 90000).ok());
    ASSERT_TRUE(muxer.write_video(out, span_of(annexb_frame(70, 0x41)), 93000, 93000, false).ok());

    // The total must be an exact multiple of the packet size, and every
    // packet must begin with the sync byte.
    EXPECT_EQ(out.size() % kPacketSize, 0u);
    auto packets = parse_ts(out);
    EXPECT_EQ(packets.size(), out.size() / kPacketSize);
}

TEST(TsMuxerTest, KeyframesCarryPcrAndRandomAccessIndicator) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    muxer.write_program_tables(out);
    ASSERT_TRUE(muxer.write_video(out, span_of(annexb_frame(400, 0x65)), 90000, 90000, true).ok());

    auto packets = parse_ts(out);
    // The first video packet of the keyframe access unit.
    bool found = false;
    for (const auto& p : packets) {
        if (p.pid == muxer.config().video_pid && p.payload_unit_start) {
            EXPECT_TRUE(p.has_pcr) << "keyframe must carry a PCR";
            EXPECT_TRUE(p.random_access) << "keyframe must set random_access_indicator";
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(TsMuxerTest, NonKeyframesDoNotCarryPcr) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    ASSERT_TRUE(muxer.write_video(out, span_of(annexb_frame(400, 0x41)), 90000, 90000, false).ok());
    for (const auto& p : parse_ts(out)) {
        EXPECT_FALSE(p.has_pcr);
        EXPECT_FALSE(p.random_access);
    }
}

TEST(TsMuxerTest, ContinuityCountersIncrementPerPidAndWrapAt16) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    // Enough small access units to force the counter past a full wrap.
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(muxer.write_video(out, span_of(annexb_frame(50, 0x41)),
                                      90000 + static_cast<std::uint64_t>(i) * 3000,
                                      90000 + static_cast<std::uint64_t>(i) * 3000, false)
                        .ok());
    }

    std::map<unsigned, std::vector<unsigned>> by_pid;
    for (const auto& p : parse_ts(out)) by_pid[p.pid].push_back(p.continuity_counter);

    ASSERT_FALSE(by_pid[muxer.config().video_pid].empty());
    const auto& counters = by_pid[muxer.config().video_pid];
    EXPECT_GT(counters.size(), 16u) << "need a wrap to be meaningful";
    for (std::size_t i = 1; i < counters.size(); ++i) {
        EXPECT_EQ(counters[i], (counters[i - 1] + 1) % 16)
            << "continuity counter discontinuity at packet " << i;
    }
}

TEST(TsMuxerTest, AudioAndVideoUseSeparatePidsAndCounters) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(muxer.write_video(out, span_of(annexb_frame(50, 0x41)), 90000, 90000, false).ok());
        ASSERT_TRUE(muxer.write_audio(out, span_of(annexb_frame(50, 0x00)), 90000).ok());
    }
    std::map<unsigned, std::vector<unsigned>> by_pid;
    for (const auto& p : parse_ts(out)) by_pid[p.pid].push_back(p.continuity_counter);

    ASSERT_EQ(by_pid.size(), 2u);
    for (const auto& [pid, counters] : by_pid) {
        for (std::size_t i = 1; i < counters.size(); ++i) {
            EXPECT_EQ(counters[i], (counters[i - 1] + 1) % 16) << "pid " << pid;
        }
    }
}

TEST(TsMuxerTest, PesHeaderCarriesCorrectlyEncodedPtsAndDts) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    const std::uint64_t pts = 123456;
    const std::uint64_t dts = 120000;
    ASSERT_TRUE(muxer.write_video(out, span_of(annexb_frame(50, 0x65)), pts, dts, true).ok());

    auto packets = parse_ts(out);
    ASSERT_FALSE(packets.empty());
    const auto& first = packets[0];
    ASSERT_TRUE(first.payload_unit_start);

    // Skip TS header + adaptation field to reach the PES.
    std::size_t p = first.offset + 4;
    if (first.has_adaptation) p += 1 + u8(out, first.offset + 4);

    // PES start code prefix and stream id.
    EXPECT_EQ(u8(out, p), 0x00u);
    EXPECT_EQ(u8(out, p + 1), 0x00u);
    EXPECT_EQ(u8(out, p + 2), 0x01u);
    EXPECT_EQ(u8(out, p + 3), 0xE0u); // video stream id

    // PTS_DTS_flags == 0b11 when a distinct DTS is present.
    EXPECT_EQ((u8(out, p + 7) >> 6) & 0x03u, 0x03u);
    EXPECT_EQ(u8(out, p + 8), 10u); // PES_header_data_length

    auto decode_ts = [&](std::size_t at) {
        return ((static_cast<std::uint64_t>(u8(out, at)) >> 1 & 0x07) << 30) |
               (static_cast<std::uint64_t>(u8(out, at + 1)) << 22) |
               ((static_cast<std::uint64_t>(u8(out, at + 2)) >> 1) << 15) |
               (static_cast<std::uint64_t>(u8(out, at + 3)) << 7) |
               (static_cast<std::uint64_t>(u8(out, at + 4)) >> 1);
    };
    EXPECT_EQ(decode_ts(p + 9), pts);
    EXPECT_EQ(decode_ts(p + 14), dts);
}

TEST(TsMuxerTest, AudioPesUsesPtsOnlyAndTheAudioStreamId) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    ASSERT_TRUE(muxer.write_audio(out, span_of(annexb_frame(50, 0x00)), 90000).ok());

    auto packets = parse_ts(out);
    ASSERT_FALSE(packets.empty());
    std::size_t p = packets[0].offset + 4;
    if (packets[0].has_adaptation) p += 1 + u8(out, packets[0].offset + 4);

    EXPECT_EQ(u8(out, p + 3), 0xC0u);                  // audio stream id
    EXPECT_EQ((u8(out, p + 7) >> 6) & 0x03u, 0x02u);   // PTS only
    EXPECT_EQ(u8(out, p + 8), 5u);
}

TEST(TsMuxerTest, EmptyAccessUnitsAreRejectedNotSilentlyEmitted) {
    TsMuxer muxer;
    std::vector<std::byte> out;
    EXPECT_FALSE(muxer.write_video(out, {}, 0, 0, true).ok());
    EXPECT_FALSE(muxer.write_audio(out, {}, 0).ok());
    EXPECT_TRUE(out.empty());
}

TEST(TsMuxerTest, ResetContinuityRestartsCountersForAReconnect) {
    TsMuxer muxer;
    std::vector<std::byte> before;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(muxer.write_video(before, span_of(annexb_frame(50, 0x41)), 90000, 90000, false).ok());
    }
    muxer.reset_continuity();

    std::vector<std::byte> after;
    ASSERT_TRUE(muxer.write_video(after, span_of(annexb_frame(50, 0x41)), 90000, 90000, false).ok());
    auto packets = parse_ts(after);
    ASSERT_FALSE(packets.empty());
    EXPECT_EQ(packets[0].continuity_counter, 0u);
}
