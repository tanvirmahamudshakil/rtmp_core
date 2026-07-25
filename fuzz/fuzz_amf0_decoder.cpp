// libFuzzer harness for protocol::amf0::decode_all (Phase 9 "fuzzing",
// docs/rtmp_promot.md). AMF0 command/data payloads arrive directly off the
// wire inside RTMP messages — decode_all() is exactly the boundary where
// attacker-controlled bytes first become structured C++ values, so it's the
// highest-value fuzz target in the protocol layer alongside the chunk
// decoder (see fuzz_chunk_decoder.cpp).
//
// Build (requires Clang; libFuzzer ships with LLVM's compiler-rt):
//   cmake --preset core-only -DRTMP_SERVER_ENABLE_FUZZING=ON
//   cmake --build --preset core-only --target fuzz_amf0_decoder
//   ./build/core-only/fuzz/fuzz_amf0_decoder -max_total_time=60
//
// Also runnable as a plain corpus replay (no libFuzzer/sanitizer runtime
// needed) via: fuzz_amf0_decoder <file> [<file> ...] — useful for replaying
// a saved crash input as a regression test without a fuzzing engine
// installed, and for hosts (like this project's macOS dev environment)
// where libFuzzer isn't readily available.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    (void)rtmp_server::protocol::amf0::decode_all(bytes);
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
