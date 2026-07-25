#pragma once

#include <cstddef>
#include <vector>

#include "rtmp_server/protocol/amf0/amf0_value.hpp"

namespace rtmp_server::protocol::amf0 {

// AMF0 wire-format markers (docs/amf0.md). Public so tests/callers that want
// to build malformed input by hand (e.g. an unknown-marker test) don't need
// to hardcode magic numbers.
enum class Amf0Marker : std::uint8_t {
    Number = 0x00,
    Boolean = 0x01,
    String = 0x02,
    Object = 0x03,
    MovieClip = 0x04, // unsupported, listed for completeness
    Null = 0x05,
    Undefined = 0x06,
    Reference = 0x07, // unsupported
    EcmaArray = 0x08,
    ObjectEnd = 0x09,
    StrictArray = 0x0A,
    Date = 0x0B,
    LongString = 0x0C,
    Unsupported = 0x0D, // unsupported
    RecordSet = 0x0E,   // unsupported
    Xml = 0x0F,          // unsupported
    TypedObject = 0x10,  // unsupported
};

// Stateless: appends the AMF0 wire encoding of `value` to `out`. Multiple
// values (e.g. command name, transaction ID, command object, arguments) are
// encoded by calling this repeatedly into the same buffer, matching how a
// single AMF0 command RTMP message payload is just the concatenation of its
// values with no envelope.
void encode(const Amf0Value& value, std::vector<std::byte>& out);

[[nodiscard]] inline std::vector<std::byte> encode(const Amf0Value& value) {
    std::vector<std::byte> out;
    encode(value, out);
    return out;
}

} // namespace rtmp_server::protocol::amf0
