#include "rtmp_server/media/flv/flv_writer.hpp"

#include <bit>
#include <cstring>
#include <iterator>
#include <string_view>

#include "rtmp_server/core/byte_order.hpp"

namespace rtmp_server::media::flv {

namespace {

using core::write_u24_be;
using core::write_u32_be;

// AMF0 markers used by the onMetaData script-data tag.
constexpr std::byte kAmf0Number{0x00};
constexpr std::byte kAmf0Boolean{0x01};
constexpr std::byte kAmf0String{0x02};
constexpr std::byte kAmf0EcmaArray{0x08};
constexpr std::byte kAmf0ObjectEndLow{0x09};

void append_u24_be(std::vector<std::byte>& out, std::uint32_t value) {
    std::array<std::byte, 3> tmp{};
    write_u24_be(tmp, value);
    out.insert(out.end(), tmp.begin(), tmp.end());
}

void append_u32_be(std::vector<std::byte>& out, std::uint32_t value) {
    std::array<std::byte, 4> tmp{};
    write_u32_be(tmp, value);
    out.insert(out.end(), tmp.begin(), tmp.end());
}

// AMF0 string body (no marker): u16 length + raw UTF-8 bytes.
void append_amf0_string_body(std::vector<std::byte>& out, std::string_view s) {
    std::array<std::byte, 2> len{};
    core::write_u16_be(len, static_cast<std::uint16_t>(s.size()));
    out.insert(out.end(), len.begin(), len.end());
    for (char c : s) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
}

// AMF0 property key (an ECMA-array/object member name) — same wire shape as a
// string body, but never prefixed with the string marker.
void append_amf0_key(std::vector<std::byte>& out, std::string_view key) {
    append_amf0_string_body(out, key);
}

void append_amf0_number(std::vector<std::byte>& out, double value) {
    out.push_back(kAmf0Number);
    auto be = encode_double_be(value);
    out.insert(out.end(), be.begin(), be.end());
}

} // namespace

std::array<std::byte, 8> encode_double_be(double value) {
    // AMF0 Numbers are network-byte-order IEEE-754 doubles.
    auto bits = std::bit_cast<std::uint64_t>(value);
    std::array<std::byte, 8> out{};
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i)] =
            static_cast<std::byte>((bits >> (56 - 8 * i)) & 0xFFU);
    }
    return out;
}

std::array<std::byte, kFileHeaderTotalSize> encode_file_header(bool has_audio, bool has_video) {
    std::array<std::byte, kFileHeaderTotalSize> h{};
    h[0] = std::byte{'F'};
    h[1] = std::byte{'L'};
    h[2] = std::byte{'V'};
    h[3] = std::byte{0x01}; // version
    std::uint8_t flags = 0;
    if (has_audio) flags |= kFlagAudio;
    if (has_video) flags |= kFlagVideo;
    h[4] = static_cast<std::byte>(flags);
    // DataOffset = 9 (size of this header); big-endian u32.
    h[5] = std::byte{0x00};
    h[6] = std::byte{0x00};
    h[7] = std::byte{0x00};
    h[8] = std::byte{0x09};
    // PreviousTagSize0 = 0.
    h[9] = std::byte{0x00};
    h[10] = std::byte{0x00};
    h[11] = std::byte{0x00};
    h[12] = std::byte{0x00};
    return h;
}

void append_tag(std::vector<std::byte>& out, std::uint8_t tag_type, std::span<const std::byte> data,
                std::uint32_t timestamp) {
    const auto data_size = static_cast<std::uint32_t>(data.size());
    out.push_back(static_cast<std::byte>(tag_type));
    append_u24_be(out, data_size);
    // Timestamp: lower 24 bits, then the extended byte holds bits 24..31.
    append_u24_be(out, timestamp & 0x00FF'FFFFU);
    out.push_back(static_cast<std::byte>((timestamp >> 24) & 0xFFU));
    // StreamID is always 0 in FLV (3 bytes).
    append_u24_be(out, 0);
    // Tag data.
    out.insert(out.end(), data.begin(), data.end());
    // PreviousTagSize = tag header (11) + data size.
    append_u32_be(out, static_cast<std::uint32_t>(kTagHeaderSize) + data_size);
}

MetadataTag build_onmetadata_tag(const OnMetaData& meta, std::uint32_t timestamp) {
    // Build the script-data payload first so we know its size, then frame it.
    std::vector<std::byte> body;

    // AMF0 String "onMetaData".
    body.push_back(kAmf0String);
    append_amf0_string_body(body, "onMetaData");

    // AMF0 ECMA array of metadata properties. The declared count is the
    // number of members; the array is still terminated by the 0x00 0x00 0x09
    // end-of-object marker (as real encoders do — the count is advisory).
    body.push_back(kAmf0EcmaArray);

    struct NumberProp {
        std::string_view key;
        double value;
    };
    const NumberProp numbers_a[] = {
        {"duration", meta.duration},
        {"width", meta.width},
        {"height", meta.height},
        {"framerate", meta.framerate},
        {"videocodecid", meta.videocodecid},
        {"audiosamplerate", meta.audiosamplerate},
        {"audiosamplesize", meta.audiosamplesize},
    };
    const NumberProp numbers_b[] = {
        {"audiocodecid", meta.audiocodecid},
        {"filesize", meta.filesize},
    };
    // Total members: numbers_a + stereo(bool) + numbers_b.
    const std::uint32_t member_count =
        static_cast<std::uint32_t>(std::size(numbers_a) + 1 + std::size(numbers_b));
    append_u32_be(body, member_count);

    std::size_t duration_off = 0;
    std::size_t filesize_off = 0;

    for (const auto& p : numbers_a) {
        append_amf0_key(body, p.key);
        body.push_back(kAmf0Number);
        if (p.key == "duration") duration_off = body.size();
        auto be = encode_double_be(p.value);
        body.insert(body.end(), be.begin(), be.end());
    }

    // stereo (boolean).
    append_amf0_key(body, "stereo");
    body.push_back(kAmf0Boolean);
    body.push_back(meta.stereo ? std::byte{0x01} : std::byte{0x00});

    for (const auto& p : numbers_b) {
        append_amf0_key(body, p.key);
        body.push_back(kAmf0Number);
        if (p.key == "filesize") filesize_off = body.size();
        auto be = encode_double_be(p.value);
        body.insert(body.end(), be.begin(), be.end());
    }

    // Object-end marker: empty key (u16 len 0) + ObjectEnd marker.
    body.push_back(std::byte{0x00});
    body.push_back(std::byte{0x00});
    body.push_back(kAmf0ObjectEndLow);

    MetadataTag out;
    append_tag(out.bytes, kTagTypeScriptData, body, timestamp);
    // The offsets recorded above are relative to `body`; the framed tag data
    // starts kTagHeaderSize bytes into out.bytes.
    out.duration_value_offset = kTagHeaderSize + duration_off;
    out.filesize_value_offset = kTagHeaderSize + filesize_off;
    return out;
}

core::Result<ParsedFlv> parse_flv(std::span<const std::byte> data) {
    using core::Error;
    using core::ErrorCategory;
    using core::ErrorCode;
    const auto fail = [](std::string_view msg) {
        return Error{ErrorCode::MalformedChunk, ErrorCategory::Storage, msg};
    };

    if (data.size() < kFileHeaderTotalSize) {
        return fail("FLV shorter than 13-byte header");
    }
    if (data[0] != std::byte{'F'} || data[1] != std::byte{'L'} || data[2] != std::byte{'V'}) {
        return fail("bad FLV signature");
    }
    ParsedFlv out;
    out.version = static_cast<std::uint8_t>(data[3]);
    const auto flags = static_cast<std::uint8_t>(data[4]);
    out.has_audio = (flags & kFlagAudio) != 0;
    out.has_video = (flags & kFlagVideo) != 0;
    out.data_offset = core::read_u32_be(std::span<const std::byte, 4>(data.subspan(5, 4)));
    // DataOffset is a client/file-controlled 32-bit length. Validating it only
    // against data.size() left a 4-byte over-read: PreviousTagSize0 is read at
    // exactly this offset, so a DataOffset in [size-3, size] passed the check
    // and then read past the end. Found by the FLV fuzz harness under ASan
    // (container-overflow in read_u32_be via parse_flv). The room for
    // PreviousTagSize0 must be part of the bound.
    //
    // Written as a subtraction rather than `data_offset + kPreviousTagSize >
    // data.size()` so a DataOffset near 2^32 cannot wrap the addition; the
    // size() >= kFileHeaderTotalSize check above guarantees the subtraction
    // cannot underflow.
    if (out.data_offset < kFileHeaderSize || out.data_offset > data.size() - kPreviousTagSize) {
        return fail("FLV DataOffset out of range");
    }
    // PreviousTagSize0 immediately follows the declared header.
    std::size_t pos = out.data_offset;
    out.previous_tag_size0 =
        core::read_u32_be(std::span<const std::byte, 4>(data.subspan(pos, 4)));
    pos += kPreviousTagSize;

    while (pos < data.size()) {
        if (pos + kTagHeaderSize > data.size()) {
            return fail("truncated FLV tag header");
        }
        ParsedTag tag;
        tag.type = static_cast<std::uint8_t>(data[pos]);
        tag.data_size = core::read_u24_be(std::span<const std::byte, 3>(data.subspan(pos + 1, 3)));
        const std::uint32_t ts_low =
            core::read_u24_be(std::span<const std::byte, 3>(data.subspan(pos + 4, 3)));
        const auto ts_ext = static_cast<std::uint32_t>(data[pos + 7]);
        tag.timestamp = (ts_ext << 24) | ts_low;
        tag.data_offset = pos + kTagHeaderSize;
        const std::size_t next = pos + kTagHeaderSize + tag.data_size;
        if (next + kPreviousTagSize > data.size()) {
            return fail("FLV tag data runs past end of file");
        }
        tag.previous_tag_size =
            core::read_u32_be(std::span<const std::byte, 4>(data.subspan(next, 4)));
        out.tags.push_back(tag);
        pos = next + kPreviousTagSize;
    }
    return out;
}

} // namespace rtmp_server::media::flv
