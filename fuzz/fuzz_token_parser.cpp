// Fuzz harness for the playback token/query parser (Phase 8 security task 4,
// "add fuzz targets for ... token parser").
//
// Covers the full path an attacker-controlled `play` stream-name argument
// takes before any signature is checked:
//
//   split_stream_name()      "name?token=..&expires=.."  -> name, query
//   parse_playback_query()   query                       -> token, expires
//   verify_token()           token, expires              -> accept / reject
//
// verify_token() is included deliberately. It is the constant-time comparison
// against a recomputed HMAC, and it is reached with a fully attacker-chosen
// token string, application, stream name and expiry — so it must tolerate
// empty, over-long, non-hex and embedded-NUL tokens without reading out of
// bounds or taking a data-dependent early exit.
//
// Build/run: see fuzz_amf0_decoder.cpp — same RTMP_SERVER_ENABLE_FUZZING
// CMake option, same standalone driver (fuzz_main.hpp) when libFuzzer is
// unavailable.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fuzz_main.hpp"
#include "rtmp_server/management/query_parser.hpp"
#include "rtmp_server/management/token.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace management = rtmp_server::management;

    const std::string_view input(reinterpret_cast<const char*>(data), size);

    const auto split = management::split_stream_name(input);
    const auto query = management::parse_playback_query(split.query);

    // Feed the parsed pieces onward exactly as RtmpAuthenticator does, so the
    // signature verification path sees attacker-shaped values rather than
    // well-formed ones.
    if (query.token.has_value()) {
        constexpr std::string_view kSecret = "fuzz-secret-not-a-real-deployment-key";
        (void)management::verify_token(kSecret, "live", split.name, *query.token, query.expires_at_unix.value_or(0),
                                        /*now_unix=*/1'700'000'000);
    }

    // Signing must also survive arbitrary application/stream names: the
    // management API builds tokens from names that originate from API callers.
    if (!split.name.empty() && split.name.size() < 4096) {
        (void)management::sign_token("fuzz-secret", "live", split.name, query.expires_at_unix.value_or(0));
    }

    // Parsing the query a second time must be deterministic and side-effect
    // free (the parser holds no state; this catches an accidental static).
    const auto again = management::parse_playback_query(split.query);
    if (again.token != query.token || again.expires_at_unix != query.expires_at_unix) {
        __builtin_trap(); // non-deterministic parse: a real defect, crash loudly
    }

    return 0;
}

namespace {

std::vector<rtmp_server_fuzz::Input> seed_corpus() {
    std::vector<rtmp_server_fuzz::Input> corpus;
    auto add = [&corpus](std::string_view s) {
        corpus.emplace_back(reinterpret_cast<const std::uint8_t*>(s.data()),
                            reinterpret_cast<const std::uint8_t*>(s.data()) + s.size());
    };

    // A realistically-shaped signed playback URL argument.
    add("mystream?token=3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b&expires=1700000000");
    add("mystream");
    add("mystream?");
    add("?token=abc");
    add("stream?expires=1700000000");
    add("stream?token=");
    add("stream?token=abc&expires=notanumber");
    add("stream?token=abc&expires=-9223372036854775808");
    add("stream?token=abc&expires=99999999999999999999999");
    // Duplicate fields — first-occurrence-wins must hold.
    add("s?token=signed&token=forged&expires=1&expires=2");
    // Junk/tracking parameters mixed in.
    add("s?utm=x&token=abc&cdn=edge&expires=1&ref=y&a=b&c=d");
    // Degenerate separators.
    add("s?&&&===&&");
    add("s?a=b=c=d");
    // Names that also stress the recording/path and stream-id layers.
    add("../../../etc/passwd?token=abc");
    add("");

    return corpus;
}

} // namespace

RTMP_SERVER_FUZZ_MAIN("fuzz_token_parser", seed_corpus)
