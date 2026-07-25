#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/protocol/amf0/amf0_value.hpp"

namespace rtmp_server::protocol::amf0 {

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
