#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"

#include <bit>

#include "rtmp_server/core/byte_order.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp" // Amf0Marker

namespace rtmp_server::protocol::amf0 {

namespace {

using core::Error;
using core::ErrorCategory;
using core::ErrorCode;
using core::Result;

Result<void> require(std::span<const std::byte> data, std::size_t offset, std::size_t needed) {
    if (offset + needed > data.size()) {
        return Error(ErrorCode::MalformedAmf, ErrorCategory::Protocol,
                      "truncated AMF0 value: not enough bytes remaining");
    }
    return Result<void>{};
}

std::uint16_t read_u16_be_at(std::span<const std::byte> data, std::size_t offset) {
    return core::read_u16_be(std::span<const std::byte, 2>(data.data() + offset, 2));
}

std::uint32_t read_u32_be_at(std::span<const std::byte> data, std::size_t offset) {
    return core::read_u32_be(std::span<const std::byte, 4>(data.data() + offset, 4));
}

double read_f64_be_at(std::span<const std::byte> data, std::size_t offset) {
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        bits = (bits << 8) | static_cast<std::uint64_t>(data[offset + i]);
    }
    return std::bit_cast<double>(bits);
}

std::string read_bytes_as_string(std::span<const std::byte> data, std::size_t offset, std::size_t len) {
    return std::string(reinterpret_cast<const char*>(data.data() + offset), len);
}

// Forward declaration: property lists (Object/ECMA Array bodies) recurse
// into decode_value() for each property's value.
Result<std::size_t> decode_properties(std::span<const std::byte> data, std::size_t offset,
                                       Amf0PropertyList& out, std::size_t depth);

// Smallest number of bytes any AMF0 value can occupy on the wire: a bare
// type marker (Null/Undefined/Object-End). Used to bound container element
// counts against the bytes actually present before reserving (Phase 8
// security task 6, "validate all client-controlled lengths before
// allocation").
constexpr std::size_t kMinValueBytes = 1;

// Decodes exactly one AMF0 value (including its leading type marker)
// starting at data[offset]. Returns the number of bytes consumed (>= 1) on
// success. `depth` is the current container nesting level; see
// kMaxNestingDepth in the header for why it must be bounded.
Result<Amf0Decoded> decode_value(std::span<const std::byte> data, std::size_t offset, std::size_t depth) {
    if (depth > kMaxNestingDepth) {
        return Error(ErrorCode::MalformedAmf, ErrorCategory::Protocol,
                     "AMF0 value nesting exceeds the maximum supported depth");
    }
    if (auto r = require(data, offset, 1); !r) return r.error();
    auto marker = static_cast<Amf0Marker>(data[offset]);
    std::size_t cursor = offset + 1;

    switch (marker) {
        case Amf0Marker::Number: {
            if (auto r = require(data, cursor, 8); !r) return r.error();
            double v = read_f64_be_at(data, cursor);
            cursor += 8;
            return Amf0Decoded{Amf0Value::number(v), cursor - offset};
        }
        case Amf0Marker::Boolean: {
            if (auto r = require(data, cursor, 1); !r) return r.error();
            bool v = data[cursor] != std::byte{0};
            cursor += 1;
            return Amf0Decoded{Amf0Value::boolean(v), cursor - offset};
        }
        case Amf0Marker::String: {
            if (auto r = require(data, cursor, 2); !r) return r.error();
            std::uint16_t len = read_u16_be_at(data, cursor);
            cursor += 2;
            if (auto r = require(data, cursor, len); !r) return r.error();
            std::string s = read_bytes_as_string(data, cursor, len);
            cursor += len;
            return Amf0Decoded{Amf0Value::string(std::move(s)), cursor - offset};
        }
        case Amf0Marker::LongString: {
            if (auto r = require(data, cursor, 4); !r) return r.error();
            std::uint32_t len = read_u32_be_at(data, cursor);
            cursor += 4;
            if (auto r = require(data, cursor, len); !r) return r.error();
            std::string s = read_bytes_as_string(data, cursor, len);
            cursor += len;
            return Amf0Decoded{Amf0Value::string(std::move(s)), cursor - offset};
        }
        case Amf0Marker::Object: {
            Amf0PropertyList props;
            auto consumed = decode_properties(data, cursor, props, depth + 1);
            if (!consumed) return consumed.error();
            cursor += consumed.value();
            return Amf0Decoded{Amf0Value::object(std::move(props)), cursor - offset};
        }
        case Amf0Marker::EcmaArray: {
            if (auto r = require(data, cursor, 4); !r) return r.error();
            cursor += 4; // associative-count hint; not authoritative, terminator governs parsing
            Amf0PropertyList props;
            auto consumed = decode_properties(data, cursor, props, depth + 1);
            if (!consumed) return consumed.error();
            cursor += consumed.value();
            return Amf0Decoded{Amf0Value::ecma_array(std::move(props)), cursor - offset};
        }
        case Amf0Marker::StrictArray: {
            if (auto r = require(data, cursor, 4); !r) return r.error();
            std::uint32_t count = read_u32_be_at(data, cursor);
            cursor += 4;
            // `count` is fully client-controlled (up to 4294967295). Reserving
            // it directly would let 5 bytes of input request ~275 GB of
            // storage. Every element needs at least kMinValueBytes on the
            // wire, so anything beyond the bytes actually remaining is
            // provably a lie — reject it up front rather than reserve-then-
            // fail (Phase 8 security task 6).
            const std::size_t remaining = data.size() - cursor;
            if (static_cast<std::size_t>(count) > remaining / kMinValueBytes) {
                return Error(ErrorCode::MalformedAmf, ErrorCategory::Protocol,
                             "AMF0 strict-array element count exceeds the bytes remaining in the message");
            }
            std::vector<Amf0Value> items;
            items.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                auto item = decode_value(data, cursor, depth + 1);
                if (!item) return item.error();
                items.push_back(std::move(item.value().value));
                cursor += item.value().bytes_consumed;
            }
            return Amf0Decoded{Amf0Value::strict_array(std::move(items)), cursor - offset};
        }
        case Amf0Marker::Date: {
            if (auto r = require(data, cursor, 10); !r) return r.error();
            double millis = read_f64_be_at(data, cursor);
            cursor += 8;
            auto tz = static_cast<std::int16_t>(read_u16_be_at(data, cursor));
            cursor += 2;
            return Amf0Decoded{Amf0Value::date(millis, tz), cursor - offset};
        }
        case Amf0Marker::Null:
            return Amf0Decoded{Amf0Value::null(), cursor - offset};
        case Amf0Marker::Undefined:
            return Amf0Decoded{Amf0Value::undefined(), cursor - offset};
        case Amf0Marker::ObjectEnd:
            return Error(ErrorCode::MalformedAmf, ErrorCategory::Protocol,
                         "unexpected AMF0 Object-End marker outside a property list");
        case Amf0Marker::MovieClip:
        case Amf0Marker::Reference:
        case Amf0Marker::Unsupported:
        case Amf0Marker::RecordSet:
        case Amf0Marker::Xml:
        case Amf0Marker::TypedObject:
            return Error(ErrorCode::MalformedAmf, ErrorCategory::Protocol,
                         "unsupported AMF0 type marker");
    }
    return Error(ErrorCode::MalformedAmf, ErrorCategory::Protocol, "unknown AMF0 type marker");
}

Result<std::size_t> decode_properties(std::span<const std::byte> data, std::size_t offset,
                                       Amf0PropertyList& out, std::size_t depth) {
    if (depth > kMaxNestingDepth) {
        return Error(ErrorCode::MalformedAmf, ErrorCategory::Protocol,
                     "AMF0 value nesting exceeds the maximum supported depth");
    }
    std::size_t cursor = offset;
    // Cap iteration so a pathological "terminator never appears" input
    // cannot spin forever; every iteration consumes at least 1 byte of
    // otherwise-empty input, so this is a defense-in-depth measure, not the
    // primary termination condition (running off the end of `data` via
    // require() is).
    constexpr std::size_t kMaxProperties = 1'000'000;
    for (std::size_t i = 0; i < kMaxProperties; ++i) {
        if (auto r = require(data, cursor, 2); !r) return r.error();
        std::uint16_t name_len = read_u16_be_at(data, cursor);
        // Empty name immediately followed by ObjectEnd marker terminates the list.
        if (name_len == 0) {
            if (auto r = require(data, cursor + 2, 1); !r) return r.error();
            if (static_cast<Amf0Marker>(data[cursor + 2]) == Amf0Marker::ObjectEnd) {
                cursor += 3;
                return cursor - offset;
            }
            // Empty property name not followed by ObjectEnd: per spec this
            // never legitimately happens (only the terminator has an empty
            // name); treat as malformed rather than silently accepting a
            // named property with an empty key.
            return Error(ErrorCode::MalformedAmf, ErrorCategory::Protocol,
                         "empty AMF0 property name not followed by Object-End marker");
        }
        cursor += 2;
        if (auto r = require(data, cursor, name_len); !r) return r.error();
        std::string name = read_bytes_as_string(data, cursor, name_len);
        cursor += name_len;

        auto value = decode_value(data, cursor, depth);
        if (!value) return value.error();
        cursor += value.value().bytes_consumed;
        out.emplace_back(std::move(name), std::move(value.value().value));
    }
    return Error(ErrorCode::MalformedAmf, ErrorCategory::Protocol,
                 "AMF0 property list exceeded sanity limit without a terminator");
}

} // namespace

core::Result<Amf0Decoded> decode(std::span<const std::byte> data) { return decode_value(data, 0, 0); }

core::Result<std::vector<Amf0Value>> decode_all(std::span<const std::byte> data) {
    std::vector<Amf0Value> values;
    std::size_t offset = 0;
    while (offset < data.size()) {
        auto decoded = decode_value(data, offset, 0);
        if (!decoded) return decoded.error();
        values.push_back(std::move(decoded.value().value));
        offset += decoded.value().bytes_consumed;
    }
    return values;
}

} // namespace rtmp_server::protocol::amf0
