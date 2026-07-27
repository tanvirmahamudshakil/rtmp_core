// libFuzzer harness for media::flv::parse_flv — the parser apps/
// flv_inspector runs on a user-supplied .flv file (Phase 9 "fuzzing",
// docs/rtmp_promot.md). Untrusted-file-input parsers deserve the same
// fuzzing coverage as untrusted-network-input parsers: a malformed on-disk
// recording (corrupted by a disk failure Phase 6 already tolerates, or
// handed to the inspector by a user) must be rejected safely, never crash.
//
// Build/run: see fuzz_amf0_decoder.cpp — same pattern.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fuzz_main.hpp"

#include "rtmp_server/media/flv/flv_writer.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    (void)rtmp_server::media::flv::parse_flv(bytes);
    return 0;
}

namespace {

// Seeds: a minimal but structurally valid FLV file (13-byte header, then
// tag/PreviousTagSize pairs). Random bytes fail the "FLV" signature check
// immediately, so mutating real headers is what reaches the tag parser.
std::vector<rtmp_server_fuzz::Input> seed_corpus() {
    std::vector<rtmp_server_fuzz::Input> corpus;

    rtmp_server_fuzz::Input flv{'F', 'L', 'V', 0x01, 0x05, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00};

    auto add_tag = [&flv](std::uint8_t type, const std::vector<std::uint8_t>& body) {
        const auto n = static_cast<std::uint32_t>(body.size());
        flv.push_back(type);
        flv.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
        flv.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        flv.push_back(static_cast<std::uint8_t>(n & 0xFF));
        flv.insert(flv.end(), {0x00, 0x00, 0x00, 0x00}); // timestamp + extended
        flv.insert(flv.end(), {0x00, 0x00, 0x00});       // stream id
        flv.insert(flv.end(), body.begin(), body.end());
        const std::uint32_t prev = n + 11;
        flv.push_back(static_cast<std::uint8_t>((prev >> 24) & 0xFF));
        flv.push_back(static_cast<std::uint8_t>((prev >> 16) & 0xFF));
        flv.push_back(static_cast<std::uint8_t>((prev >> 8) & 0xFF));
        flv.push_back(static_cast<std::uint8_t>(prev & 0xFF));
    };

    corpus.push_back(flv); // header only
    add_tag(0x12, {0x02, 0x00, 0x0A, 'o', 'n', 'M', 'e', 't', 'a', 'D', 'a', 't', 'a'}); // script
    corpus.push_back(flv);
    add_tag(0x09, {0x17, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64, 0x00, 0x1F}); // AVC sequence header
    add_tag(0x08, {0xAF, 0x00, 0x12, 0x10});                               // AAC sequence header
    add_tag(0x09, {0x27, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x09, 0x30}); // AVC NALU
    corpus.push_back(flv);

    corpus.push_back({'F', 'L', 'V'});
    corpus.push_back({});

    return corpus;
}

} // namespace

RTMP_SERVER_FUZZ_MAIN("fuzz_flv_parser", seed_corpus)
