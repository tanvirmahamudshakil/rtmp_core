#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rtmp_server::management {

// Bounded parser for the playback-URL query string
// ("?token=<hex>&expires=<unix>") that arrives inside an RTMP `play`
// command's stream-name argument (Phase 8 security tasks 4, 5 and 6).
//
// Why this exists as its own unit. The parser previously lived in an
// anonymous namespace inside src/authentication/rtmp_authenticator.cpp,
// which meant (a) it could not be fuzzed or unit-tested directly, and (b) it
// was unbounded: it built an std::unordered_map with one entry per "&"
// separated pair, from a string whose only ceiling was the RTMP message size
// limit (10 MiB by default). A single `play` command of "a=b&a=b&..." could
// therefore force ~2.5 million map insertions and the allocations that go
// with them, before any authentication had succeeded.
//
// This parser is allocation-light and hard-bounded: it scans for exactly the
// two fields the token scheme defines and ignores everything else, so its
// cost is O(query length) with no per-parameter allocation and no map.
// Unknown parameters are not an error — deployments put tracking/CDN
// parameters on playback URLs — they are simply skipped.

// Longest query string accepted. A signed token is 64 hex characters and a
// unix expiry is 10-11 digits, so a legitimate query is well under 200 bytes;
// 1024 leaves generous room for extra deployment parameters while keeping the
// scan cost trivially bounded.
inline constexpr std::size_t kMaxQueryLength = 1024;

// Longest accepted value for a single recognised field. The token is
// HMAC-SHA256 hex (64 chars); anything materially longer cannot be a valid
// signature, so there is no reason to copy it.
inline constexpr std::size_t kMaxFieldValueLength = 128;

// Largest number of "&"-separated pairs scanned before the parser stops.
// Bounds worst-case work on a pathological query independently of
// kMaxQueryLength.
inline constexpr std::size_t kMaxQueryPairs = 32;

struct PlaybackQuery {
    // Present only if a syntactically plausible "token=" was found: non-empty
    // and no longer than kMaxFieldValueLength. Never validated
    // cryptographically here — that is StreamManager::verify_playback_token's
    // job.
    std::optional<std::string> token;

    // Present only if "expires=" parsed as a complete signed decimal integer.
    // A malformed or overflowing value yields std::nullopt rather than 0, so
    // callers can distinguish "no expiry supplied" from "expiry is the epoch"
    // — the previous code used std::from_chars and silently left 0 on
    // failure, turning a garbled expiry into a definitely-expired token,
    // which is fail-closed but indistinguishable in logs.
    std::optional<std::int64_t> expires_at_unix;

    // True if the input exceeded kMaxQueryLength or kMaxQueryPairs and was
    // therefore only partially scanned. Callers that mandate tokens should
    // treat this as a rejection.
    bool truncated = false;
};

// Parses `query` (the part after '?', without the leading '?'). Never throws,
// never allocates more than the two extracted values, and performs no
// percent-decoding — the values this project puts on the wire (hex
// signatures, decimal timestamps) never need it, and skipping it avoids a
// whole class of decoding bugs on attacker input.
[[nodiscard]] PlaybackQuery parse_playback_query(std::string_view query);

// Splits an RTMP stream-name argument such as "mystream?token=..&expires=.."
// into its name and query halves. Returns the whole input as the name and an
// empty query when there is no '?'.
struct SplitStreamName {
    std::string_view name;
    std::string_view query;
};
[[nodiscard]] SplitStreamName split_stream_name(std::string_view stream_arg) noexcept;

} // namespace rtmp_server::management
