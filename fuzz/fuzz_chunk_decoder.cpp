// libFuzzer harness for protocol::chunk::ChunkDecoder — the very first
// stateful parser attacker-controlled RTMP bytes reach after the handshake
// (Phase 9 "fuzzing", docs/rtmp_promot.md). Splits the input into two
// halves fed as two separate on_bytes_received() calls, to also exercise
// the cross-call partial-message reassembly path (a single fuzz corpus
// entry can't otherwise reach state carried between calls), matching how a
// real socket delivers RTMP bytes in arbitrary fragments.
//
// Build/run: see fuzz_amf0_decoder.cpp — same pattern, same
// RTMP_SERVER_ENABLE_FUZZING CMake option, same standalone-corpus-replay
// fallback via `main()` when libFuzzer isn't linked.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    rtmp_server::protocol::chunk::ChunkDecoder decoder(/*max_message_size=*/10 * 1024 * 1024);
    decoder.set_message_handler([](rtmp_server::protocol::chunk::RtmpMessage) {});
    decoder.set_error_handler([](rtmp_server::core::Error) {});

    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    std::size_t half = bytes.size() / 2;
    decoder.on_bytes_received(bytes.first(half));
    decoder.on_bytes_received(bytes.subspan(half));
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
