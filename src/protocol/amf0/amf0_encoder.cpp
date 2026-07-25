#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"

#include <array>
#include <bit>
#include <cstring>

#include "rtmp_server/core/byte_order.hpp"

namespace rtmp_server::protocol::amf0 {

namespace {

void append_u8(std::vector<std::byte>& out, std::uint8_t v) { out.push_back(static_cast<std::byte>(v)); }

void append_marker(std::vector<std::byte>& out, Amf0Marker m) { append_u8(out, static_cast<std::uint8_t>(m)); }

void append_u16_be(std::vector<std::byte>& out, std::uint16_t v) {
    std::array<std::byte, 2> tmp{};
    core::write_u16_be(tmp, v);
    out.insert(out.end(), tmp.begin(), tmp.end());
}

void append_u32_be(std::vector<std::byte>& out, std::uint32_t v) {
    std::array<std::byte, 4> tmp{};
    core::write_u32_be(tmp, v);
    out.insert(out.end(), tmp.begin(), tmp.end());
}

void append_f64_be(std::vector<std::byte>& out, double v) {
    std::uint64_t bits = std::bit_cast<std::uint64_t>(v);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((bits >> shift) & 0xFF));
    }
}

// AMF0 "String" (u16 length) is used for strings up to 65535 bytes; longer
// strings must use the "Long String" marker (u32 length) per spec. This is
// purely a wire-encoding choice driven by length — the value model does not
// distinguish the two (see amf0_value.hpp).
void append_string_body(std::vector<std::byte>& out, const std::string& s) {
    if (s.size() <= 0xFFFF) {
        append_u16_be(out, static_cast<std::uint16_t>(s.size()));
    } else {
        append_u32_be(out, static_cast<std::uint32_t>(s.size()));
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(s.data());
    out.insert(out.end(), bytes, bytes + s.size());
}

void append_property_name(std::vector<std::byte>& out, const std::string& name) {
    // Property names inside Object/ECMA Array are always the *short* u16-length
    // form on the wire, even if (pathologically) longer than 65535 bytes.
    append_u16_be(out, static_cast<std::uint16_t>(name.size() & 0xFFFF));
    const auto* bytes = reinterpret_cast<const std::byte*>(name.data());
    out.insert(out.end(), bytes, bytes + name.size());
}

void append_properties(std::vector<std::byte>& out, const Amf0PropertyList& properties) {
    for (const auto& [name, value] : properties) {
        append_property_name(out, name);
        encode(value, out);
    }
    // Terminator: empty (u16=0) property name followed by the ObjectEnd marker.
    append_u16_be(out, 0);
    append_marker(out, Amf0Marker::ObjectEnd);
}

} // namespace

void encode(const Amf0Value& value, std::vector<std::byte>& out) {
    switch (value.type()) {
        case Amf0Type::Number:
            append_marker(out, Amf0Marker::Number);
            append_f64_be(out, value.as_number());
            return;
        case Amf0Type::Boolean:
            append_marker(out, Amf0Marker::Boolean);
            append_u8(out, value.as_boolean() ? 1 : 0);
            return;
        case Amf0Type::String:
            if (value.as_string().size() <= 0xFFFF) {
                append_marker(out, Amf0Marker::String);
            } else {
                append_marker(out, Amf0Marker::LongString);
            }
            append_string_body(out, value.as_string());
            return;
        case Amf0Type::Object:
            append_marker(out, Amf0Marker::Object);
            append_properties(out, value.as_object());
            return;
        case Amf0Type::Null:
            append_marker(out, Amf0Marker::Null);
            return;
        case Amf0Type::Undefined:
            append_marker(out, Amf0Marker::Undefined);
            return;
        case Amf0Type::EcmaArray:
            append_marker(out, Amf0Marker::EcmaArray);
            append_u32_be(out, static_cast<std::uint32_t>(value.as_ecma_array().size()));
            append_properties(out, value.as_ecma_array());
            return;
        case Amf0Type::StrictArray: {
            append_marker(out, Amf0Marker::StrictArray);
            const auto& items = value.as_strict_array();
            append_u32_be(out, static_cast<std::uint32_t>(items.size()));
            for (const auto& item : items) encode(item, out);
            return;
        }
        case Amf0Type::Date:
            append_marker(out, Amf0Marker::Date);
            append_f64_be(out, value.as_date().milliseconds);
            append_u16_be(out, static_cast<std::uint16_t>(value.as_date().timezone));
            return;
    }
}

} // namespace rtmp_server::protocol::amf0
