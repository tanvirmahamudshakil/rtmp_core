#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/protocol/amf0/amf0_value.hpp"

namespace rtmp_server::protocol::amf0 {

// Maximum Object/ECMA-Array/Strict-Array nesting the decoder will descend
// into before rejecting the input (docs/v2_promot.md 3.5 "AMF nesting
// depth", Phase 8 security task 5).
//
// The decoder is recursive-descent, so nesting depth is *stack* depth: an
// unbounded limit means an attacker who can send an AMF0 command message
// (i.e. any peer that finished the RTMP handshake — no authentication
// required) crashes the process with a stack overflow. Phase 8 reproduced
// exactly that: 800 KB of `03 00 01 61` repeated (object, one-char property
// name, value = another object) segfaulted the decoder before this limit
// existed. See docs/security.md "AMF0 nesting depth".
//
// 32 is far above anything real: OBS/FFmpeg/librtmp `connect` command
// objects nest at most 2-3 levels, and the deepest structure the server
// itself ever emits (onMetaData with a nested keyframes array) is 3.
inline constexpr std::size_t kMaxNestingDepth = 32;

// Result of decoding exactly one AMF0 value off the front of a byte span.
struct Amf0Decoded {
    Amf0Value value;
    std::size_t bytes_consumed = 0;
};

// Decodes exactly one AMF0 value starting at data[0]. On success,
// bytes_consumed reports how many bytes of `data` the value occupied (the
// caller advances by that much to decode the next value, if any — mirrors
// ChunkDecoder's cursor-based style). Fails with
// core::ErrorCode::MalformedAmf on truncated input, an unknown/unsupported
// type marker, or a structurally invalid Object/ECMA Array (missing/garbled
// terminator). Never throws.
[[nodiscard]] core::Result<Amf0Decoded> decode(std::span<const std::byte> data);

// Decodes every AMF0 value in `data` back to back until the span is fully
// consumed (this is the shape of a whole AMF0 command/data RTMP message
// payload: command name, transaction ID, command object, then zero or more
// argument values, with no outer envelope or count prefix). Fails the same
// way decode() does; also fails if a trailing partial value is left over.
[[nodiscard]] core::Result<std::vector<Amf0Value>> decode_all(std::span<const std::byte> data);

} // namespace rtmp_server::protocol::amf0
