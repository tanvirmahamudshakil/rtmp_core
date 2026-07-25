#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <vector>

#include "rtmp_server/media/flv/flv_writer.hpp"

// flv_inspector <file.flv>
//
// Reads an FLV file produced by the recorder (or any FLV muxer) and prints
// the file header plus every tag (type, timestamp, data size). Doubles as the
// acceptance evidence for "recorded FLV is playable": with no real media
// player on this host, we validate the container byte-for-byte by parsing it
// back with the same spec-derived parser (docs/phase6-checklist.md).

namespace {

const char* tag_type_name(std::uint8_t type) {
    switch (type) {
        case rtmp_server::media::flv::kTagTypeAudio: return "audio";
        case rtmp_server::media::flv::kTagTypeVideo: return "video";
        case rtmp_server::media::flv::kTagTypeScriptData: return "script";
        default: return "unknown";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <file.flv>\n", argv[0]);
        return EXIT_FAILURE;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> data(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        data[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }

    auto parsed = rtmp_server::media::flv::parse_flv(
        std::span<const std::byte>(data.data(), data.size()));
    if (!parsed.ok()) {
        std::fprintf(stderr, "not a valid FLV: %s\n", parsed.error().message().c_str());
        return EXIT_FAILURE;
    }

    const auto& flv = parsed.value();
    std::printf("FLV version=%u audio=%s video=%s data_offset=%u prev_tag_size0=%u bytes=%zu\n",
                flv.version, flv.has_audio ? "yes" : "no", flv.has_video ? "yes" : "no",
                flv.data_offset, flv.previous_tag_size0, data.size());
    std::printf("%-4s %-7s %-12s %-10s %-10s\n", "#", "type", "timestamp", "data_size", "prev_size");
    std::size_t idx = 0;
    for (const auto& tag : flv.tags) {
        std::printf("%-4zu %-7s %-12u %-10u %-10u\n", idx++, tag_type_name(tag.type), tag.timestamp,
                    tag.data_size, tag.previous_tag_size);
    }
    std::printf("total tags: %zu\n", flv.tags.size());
    return EXIT_SUCCESS;
}
