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
#include <span>
#include <vector>

#include "fuzz_main.hpp"
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

namespace {

// Seeds are well-formed chunk streams: a random byte string almost always
// fails the fmt/csid checks in the first few bytes, so mutating real chunk
// headers is what actually reaches the reassembly and protocol-control paths.
std::vector<rtmp_server_fuzz::Input> seed_corpus() {
    std::vector<rtmp_server_fuzz::Input> corpus;

    auto fmt0 = [](std::uint8_t csid, std::uint32_t len, std::uint8_t type, std::size_t payload) {
        rtmp_server_fuzz::Input c{csid};
        c.insert(c.end(), {0, 0, 0});                                                       // timestamp
        c.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
        c.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
        c.push_back(static_cast<std::uint8_t>(len & 0xFF));
        c.push_back(type);
        c.insert(c.end(), {0, 0, 0, 0});                                                    // message stream id
        for (std::size_t i = 0; i < payload; ++i) c.push_back(0x42);
        return c;
    };

    corpus.push_back(fmt0(0x03, 4, 0x14, 4));   // AMF0 command, complete
    corpus.push_back(fmt0(0x04, 256, 0x09, 128)); // video, spans two chunks
    corpus.push_back(fmt0(0x05, 64, 0x08, 64));   // audio, complete

    // Set Chunk Size (protocol control, type 1) -> exercises the negotiated
    // chunk-size bound.
    auto set_chunk_size = fmt0(0x02, 4, 0x01, 0);
    set_chunk_size.insert(set_chunk_size.end(), {0x00, 0x00, 0x10, 0x00});
    corpus.push_back(set_chunk_size);

    // Abort Message (type 2) -> exercises the reassembly-release path.
    auto abort = fmt0(0x02, 4, 0x02, 0);
    abort.insert(abort.end(), {0x00, 0x00, 0x00, 0x03});
    corpus.push_back(abort);

    // Window Acknowledgement Size (type 5).
    auto window = fmt0(0x02, 4, 0x05, 0);
    window.insert(window.end(), {0x00, 0x25, 0x00, 0x00});
    corpus.push_back(window);

    // Extended timestamp (0xFFFFFF escape) -> the rollover arithmetic path.
    rtmp_server_fuzz::Input extended{0x03, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x04, 0x09, 0, 0, 0, 0,
                                     0xFF, 0xFF, 0xFF, 0xF0, 0x11, 0x22, 0x33, 0x44};
    corpus.push_back(extended);

    // fmt1 / fmt2 / fmt3 continuations following a fmt0 opener.
    auto interleaved = fmt0(0x03, 300, 0x09, 128);
    interleaved.insert(interleaved.end(), {0xC3});                       // fmt3 continuation, csid 3
    for (int i = 0; i < 128; ++i) interleaved.push_back(0x43);
    interleaved.insert(interleaved.end(), {0x43, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x08, 0x08}); // fmt1
    corpus.push_back(interleaved);

    return corpus;
}

} // namespace

RTMP_SERVER_FUZZ_MAIN("fuzz_chunk_decoder", seed_corpus)
