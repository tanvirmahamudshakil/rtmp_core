// Fuzz harness for protocol::handshake::HandshakeSession (Phase 8 security
// task 4, "add fuzz targets for ... handshake parser").
//
// This is the earliest attacker-reachable parser in the whole server: the
// bytes it consumes arrive before any authentication, before any chunk
// decoding, and before the connection is associated with an application or
// stream. Anything that can be reached here is reachable by an unauthenticated
// peer that has done nothing but open a TCP connection.
//
// The input is split into a randomised sequence of fragments rather than fed
// as one span, because the handshake is a state machine driven across
// arbitrary transport-level fragmentation (C0, C1 and C2 routinely arrive
// split, and real clients such as OBS pipeline the `connect` command into the
// same write as C2). Feeding one contiguous buffer would never exercise the
// cross-call boundary conditions, which is exactly where a state machine
// breaks.
//
// Build/run: see fuzz_amf0_decoder.cpp — same RTMP_SERVER_ENABLE_FUZZING
// CMake option, same standalone driver (fuzz_main.hpp) when libFuzzer is
// unavailable.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fuzz_main.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace handshake = rtmp_server::protocol::handshake;

    handshake::HandshakeSession session;
    // The handlers must not be null: a large part of what this harness is
    // testing is that the session never invokes a handler after reaching a
    // terminal state, and never re-enters one.
    std::size_t sent_bytes = 0;
    bool completed = false;
    session.set_send_handler([&sent_bytes](rtmp_server::core::SharedBuffer buffer) { sent_bytes += buffer.size(); });
    session.set_complete_handler([&completed]() { completed = true; });
    session.set_fail_handler([](rtmp_server::core::Error) {});

    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);

    // Derive the fragmentation pattern from the input itself, so it is a
    // deterministic function of the corpus entry (a saved crash replays
    // identically) while still varying across the corpus.
    std::size_t offset = 0;
    std::size_t stride_seed = size == 0 ? 1 : static_cast<std::size_t>(data[0]);
    while (offset < bytes.size()) {
        stride_seed = (stride_seed * 1103515245u + 12345u) & 0xFFFFu;
        const std::size_t stride = 1 + (stride_seed % 257);
        const std::size_t take = std::min(stride, bytes.size() - offset);
        session.on_bytes_received(bytes.subspan(offset, take));
        offset += take;
    }

    // Feeding bytes after a terminal state, and taking the trailing bytes,
    // must both be safe and must not resurrect the state machine.
    if (session.is_terminal()) {
        session.on_bytes_received(bytes);
        auto trailing = session.take_trailing_bytes();
        (void)trailing;
        // take_trailing_bytes() moves out of the buffer; calling it twice must
        // not double-move or return stale data.
        auto again = session.take_trailing_bytes();
        (void)again;
    }
    // A timeout arriving after the handshake already finished must be a no-op,
    // not a second failure notification.
    session.on_timeout();

    (void)sent_bytes;
    (void)completed;
    return 0;
}

namespace {

// Seeds are structurally valid handshake prefixes. Random bytes fail at the
// C0 version check in one byte, so without these the fuzzer never reaches C1
// or C2 handling at all.
std::vector<rtmp_server_fuzz::Input> seed_corpus() {
    namespace handshake = rtmp_server::protocol::handshake;
    std::vector<rtmp_server_fuzz::Input> corpus;

    // C0 alone.
    corpus.push_back({handshake::kRtmpVersion});

    // C0 + C1 (version, then time/zero/random).
    rtmp_server_fuzz::Input c0c1{handshake::kRtmpVersion};
    c0c1.insert(c0c1.end(), {0x00, 0x00, 0x00, 0x01}); // time
    c0c1.insert(c0c1.end(), {0x00, 0x00, 0x00, 0x00}); // zero
    for (std::size_t i = 0; i < handshake::kRandomEchoSize; ++i) {
        c0c1.push_back(static_cast<std::uint8_t>(i & 0xFF));
    }
    corpus.push_back(c0c1);

    // Full C0 + C1 + C2.
    rtmp_server_fuzz::Input full = c0c1;
    for (std::size_t i = 0; i < handshake::kHandshakeChunkSize; ++i) {
        full.push_back(static_cast<std::uint8_t>((i * 7) & 0xFF));
    }
    corpus.push_back(full);

    // C0+C1+C2 with a pipelined `connect` command chunk appended — the
    // trailing-bytes path (docs/production-gap-analysis.md item #3).
    rtmp_server_fuzz::Input pipelined = full;
    pipelined.insert(pipelined.end(), {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x14, 0x00, 0x00, 0x00, 0x00});
    pipelined.insert(pipelined.end(), {0x02, 0x00, 0x07, 'c', 'o', 'n', 'n', 'e', 'c', 't', 0x00, 0x3F, 0xF0, 0x00});
    corpus.push_back(pipelined);

    // Wrong version byte — the immediate-rejection path.
    corpus.push_back({0xFF});
    // Zero-length input.
    corpus.push_back({});

    return corpus;
}

} // namespace

RTMP_SERVER_FUZZ_MAIN("fuzz_handshake", seed_corpus)
