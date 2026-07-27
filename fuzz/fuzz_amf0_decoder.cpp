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
#include <span>
#include <vector>

#include "fuzz_main.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    (void)rtmp_server::protocol::amf0::decode_all(bytes);
    return 0;
}

namespace {

// Structure-aware seeds: real AMF0 command payloads. Random bytes are
// rejected by the very first type-marker switch, so without these the fuzzer
// would only ever exercise the "unknown marker" error path.
std::vector<rtmp_server_fuzz::Input> seed_corpus() {
    namespace amf0 = rtmp_server::protocol::amf0;
    std::vector<rtmp_server_fuzz::Input> corpus;

    auto emit = [&corpus](const std::vector<amf0::Amf0Value>& values) {
        std::vector<std::byte> encoded;
        for (const auto& v : values) amf0::encode(v, encoded);
        rtmp_server_fuzz::Input bytes(encoded.size());
        for (std::size_t i = 0; i < encoded.size(); ++i) bytes[i] = static_cast<std::uint8_t>(encoded[i]);
        corpus.push_back(std::move(bytes));
    };

    // NetConnection.connect
    amf0::Amf0PropertyList connect_obj;
    connect_obj.emplace_back("app", amf0::Amf0Value::string("live"));
    connect_obj.emplace_back("tcUrl", amf0::Amf0Value::string("rtmp://host/live"));
    connect_obj.emplace_back("objectEncoding", amf0::Amf0Value::number(0));
    emit({amf0::Amf0Value::string("connect"), amf0::Amf0Value::number(1),
          amf0::Amf0Value::object(std::move(connect_obj))});

    // publish / play
    emit({amf0::Amf0Value::string("publish"), amf0::Amf0Value::number(2), amf0::Amf0Value::null(),
          amf0::Amf0Value::string("streamkey"), amf0::Amf0Value::string("live")});
    emit({amf0::Amf0Value::string("play"), amf0::Amf0Value::number(3), amf0::Amf0Value::null(),
          amf0::Amf0Value::string("stream?token=abc&expires=1700000000")});

    // onMetaData: an ECMA array with a nested strict array, the deepest
    // structure a real publisher sends.
    std::vector<amf0::Amf0Value> keyframes;
    for (int i = 0; i < 4; ++i) keyframes.push_back(amf0::Amf0Value::number(i));
    amf0::Amf0PropertyList meta;
    meta.emplace_back("duration", amf0::Amf0Value::number(0));
    meta.emplace_back("width", amf0::Amf0Value::number(1920));
    meta.emplace_back("videocodecid", amf0::Amf0Value::number(7));
    meta.emplace_back("stereo", amf0::Amf0Value::boolean(true));
    meta.emplace_back("keyframes", amf0::Amf0Value::strict_array(std::move(keyframes)));
    emit({amf0::Amf0Value::string("@setDataFrame"), amf0::Amf0Value::string("onMetaData"),
          amf0::Amf0Value::ecma_array(std::move(meta))});

    // Every bare marker, so the mutator can graft one anywhere.
    corpus.push_back({0x05});                                     // null
    corpus.push_back({0x06});                                     // undefined
    corpus.push_back({0x0A, 0x00, 0x00, 0x00, 0x02, 0x05, 0x05}); // strict array of 2 nulls
    corpus.push_back({0x03, 0x00, 0x00, 0x09});                   // empty object

    return corpus;
}

} // namespace

RTMP_SERVER_FUZZ_MAIN("fuzz_amf0_decoder", seed_corpus)
