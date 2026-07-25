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
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#include "rtmp_server/media/flv/flv_writer.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    (void)rtmp_server::media::flv::parse_flv(bytes);
    return 0;
}

#ifndef RTMP_SERVER_FUZZING_ENGINE_LIBFUZZER
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        LLVMFuzzerTestOneInput(data.data(), data.size());
    }
    return 0;
}
#endif
