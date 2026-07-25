#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rtmp_server::protocol::amf0 {

// AMF0 value model (docs/amf0.md has the full write-up; AMF0 is specified by
// Adobe's "amf0-file-format-specification.pdf"). Supports every AMF0 type
// this server needs to speak RTMP command messages: Number, Boolean, String
// (incl. the Long String wire form — same value type, the encoder picks the
// marker based on length), Object, Null, Undefined, ECMA Array, Strict
// Array, and Date. Reference/Unsupported/XML/Typed-Object markers are not
// needed by connect/createStream/publish/play and are intentionally not
// modeled.
//
// `Object` and `EcmaArray` are distinguished as separate std::variant
// alternative types (not a shared type + tag) so the variant's own
// discriminant is enough to recover the AMF0 wire type.
//
// Object/ECMA-Array/Strict-Array bodies are recursive (they contain
// Amf0Value). A directly-nested std::vector<std::pair<std::string,
// Amf0Value>> data member cannot be made to work here: std::pair (unlike
// std::vector/list/forward_list, see P0307) is not guaranteed usable with
// an incomplete element type, and the *first* implicit instantiation of
// vector<pair<string, Amf0Value>> within this header (while Amf0Value is
// still being defined) would permanently poison that instantiation for the
// whole translation unit per the one-definition rule, breaking use sites
// far away (e.g. test files) where Amf0Value is otherwise complete. Storing
// the recursive containers behind a std::shared_ptr breaks the cycle
// cleanly: shared_ptr<T>'s layout never depends on T's completeness, and
// its deleter is only instantiated where the pointee is actually
// constructed (the factory functions below, written after Amf0Value is a
// complete type). Values are treated as immutable once constructed, so
// sharing the pointee across copies is safe and avoids a deep copy on
// every Amf0Value copy.
class Amf0Value;

// Ordered list of name/value pairs. AMF0 Object and ECMA Array both use this
// shape on the wire (a run of `(u16 name-length, name, value)` triples). A
// vector (not a map) is used deliberately: it preserves encounter order,
// which is what round-trip tests (and most real encoders) expect, and RTMP
// command objects/args are always small.
using Amf0PropertyList = std::vector<std::pair<std::string, Amf0Value>>;
using Amf0ValueList = std::vector<Amf0Value>;

struct Amf0Null {
    bool operator==(const Amf0Null&) const noexcept = default;
};

struct Amf0Undefined {
    bool operator==(const Amf0Undefined&) const noexcept = default;
};

struct Amf0Object {
    std::shared_ptr<const Amf0PropertyList> properties;
    bool operator==(const Amf0Object& other) const;
};

struct Amf0EcmaArray {
    std::shared_ptr<const Amf0PropertyList> properties;
    bool operator==(const Amf0EcmaArray& other) const;
};

struct Amf0StrictArray {
    std::shared_ptr<const Amf0ValueList> items;
    bool operator==(const Amf0StrictArray& other) const;
};

// AMF0 Date: milliseconds since the Unix epoch (may be fractional per spec,
// though RTMP peers always send integral values in practice) plus a
// timezone field that is always written as 0 by conforming encoders (Adobe
// deprecated non-zero timezones) and is preserved verbatim on decode.
struct Amf0Date {
    double milliseconds = 0.0;
    std::int16_t timezone = 0;
    bool operator==(const Amf0Date&) const noexcept = default;
};

enum class Amf0Type : std::uint8_t {
    Number,
    Boolean,
    String,
    Object,
    Null,
    Undefined,
    EcmaArray,
    StrictArray,
    Date,
};

class Amf0Value {
public:
    Amf0Value() : storage_(Amf0Undefined{}) {}

    static Amf0Value number(double v) { return Amf0Value(v); }
    static Amf0Value boolean(bool v) { return Amf0Value(BoolTag{v}); }
    static Amf0Value string(std::string v) { return Amf0Value(std::move(v)); }
    static Amf0Value object(Amf0PropertyList properties = {}) {
        return Amf0Value(Amf0Object{std::make_shared<const Amf0PropertyList>(std::move(properties))});
    }
    static Amf0Value null() { return Amf0Value(Amf0Null{}); }
    static Amf0Value undefined() { return Amf0Value(Amf0Undefined{}); }
    static Amf0Value ecma_array(Amf0PropertyList properties = {}) {
        return Amf0Value(Amf0EcmaArray{std::make_shared<const Amf0PropertyList>(std::move(properties))});
    }
    static Amf0Value strict_array(Amf0ValueList items = {}) {
        return Amf0Value(Amf0StrictArray{std::make_shared<const Amf0ValueList>(std::move(items))});
    }
    static Amf0Value date(double milliseconds, std::int16_t timezone = 0) {
        return Amf0Value(Amf0Date{milliseconds, timezone});
    }

    [[nodiscard]] Amf0Type type() const noexcept {
        return std::visit(
            [](const auto& v) -> Amf0Type {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, double>) return Amf0Type::Number;
                else if constexpr (std::is_same_v<T, BoolTag>) return Amf0Type::Boolean;
                else if constexpr (std::is_same_v<T, std::string>) return Amf0Type::String;
                else if constexpr (std::is_same_v<T, Amf0Object>) return Amf0Type::Object;
                else if constexpr (std::is_same_v<T, Amf0Null>) return Amf0Type::Null;
                else if constexpr (std::is_same_v<T, Amf0Undefined>) return Amf0Type::Undefined;
                else if constexpr (std::is_same_v<T, Amf0EcmaArray>) return Amf0Type::EcmaArray;
                else if constexpr (std::is_same_v<T, Amf0StrictArray>) return Amf0Type::StrictArray;
                else if constexpr (std::is_same_v<T, Amf0Date>) return Amf0Type::Date;
            },
            storage_);
    }

    [[nodiscard]] bool is_number() const noexcept { return type() == Amf0Type::Number; }
    [[nodiscard]] bool is_boolean() const noexcept { return type() == Amf0Type::Boolean; }
    [[nodiscard]] bool is_string() const noexcept { return type() == Amf0Type::String; }
    [[nodiscard]] bool is_object() const noexcept { return type() == Amf0Type::Object; }
    [[nodiscard]] bool is_null() const noexcept { return type() == Amf0Type::Null; }
    [[nodiscard]] bool is_undefined() const noexcept { return type() == Amf0Type::Undefined; }
    [[nodiscard]] bool is_ecma_array() const noexcept { return type() == Amf0Type::EcmaArray; }
    [[nodiscard]] bool is_strict_array() const noexcept { return type() == Amf0Type::StrictArray; }
    [[nodiscard]] bool is_date() const noexcept { return type() == Amf0Type::Date; }

    // Accessors assert the expected type (a request for the wrong
    // alternative is a programming error in the caller, not an expected
    // failure mode — malformed *wire* input is rejected by the decoder
    // before an Amf0Value is ever produced).
    [[nodiscard]] double as_number() const { return std::get<double>(storage_); }
    [[nodiscard]] bool as_boolean() const { return std::get<BoolTag>(storage_).value; }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(storage_); }
    [[nodiscard]] const Amf0PropertyList& as_object() const {
        return *std::get<Amf0Object>(storage_).properties;
    }
    [[nodiscard]] const Amf0PropertyList& as_ecma_array() const {
        return *std::get<Amf0EcmaArray>(storage_).properties;
    }
    [[nodiscard]] const Amf0ValueList& as_strict_array() const {
        return *std::get<Amf0StrictArray>(storage_).items;
    }
    [[nodiscard]] const Amf0Date& as_date() const { return std::get<Amf0Date>(storage_); }

    // Looks up `key` in an Object or ECMA Array's property list (first
    // match wins, matching how a real AMF0 object with a duplicate key
    // would be interpreted by most decoders). Returns nullptr if this value
    // is not Object/EcmaArray or the key is absent.
    [[nodiscard]] const Amf0Value* find(std::string_view key) const noexcept {
        const Amf0PropertyList* props = nullptr;
        if (auto* obj = std::get_if<Amf0Object>(&storage_)) props = obj->properties.get();
        else if (auto* arr = std::get_if<Amf0EcmaArray>(&storage_)) props = arr->properties.get();
        if (props == nullptr) return nullptr;
        for (const auto& [name, value] : *props) {
            if (name == key) return &value;
        }
        return nullptr;
    }

    bool operator==(const Amf0Value& other) const = default;

private:
    // Distinct wrapper so `variant::index()`/visit can tell Boolean apart
    // from other alternatives without relying on bool's implicit
    // convertibility to/from numeric types confusing overload resolution.
    struct BoolTag {
        bool value;
        bool operator==(const BoolTag&) const noexcept = default;
    };

    explicit Amf0Value(double v) : storage_(v) {}
    explicit Amf0Value(BoolTag v) : storage_(v) {}
    explicit Amf0Value(std::string v) : storage_(std::move(v)) {}
    explicit Amf0Value(Amf0Object v) : storage_(std::move(v)) {}
    explicit Amf0Value(Amf0Null v) : storage_(v) {}
    explicit Amf0Value(Amf0Undefined v) : storage_(v) {}
    explicit Amf0Value(Amf0EcmaArray v) : storage_(std::move(v)) {}
    explicit Amf0Value(Amf0StrictArray v) : storage_(std::move(v)) {}
    explicit Amf0Value(Amf0Date v) : storage_(v) {}

    std::variant<double, BoolTag, std::string, Amf0Object, Amf0Null, Amf0Undefined, Amf0EcmaArray,
                 Amf0StrictArray, Amf0Date>
        storage_;
};

inline bool Amf0Object::operator==(const Amf0Object& other) const {
    if (properties == other.properties) return true; // same pointer (or both null)
    if (!properties || !other.properties) return false;
    return *properties == *other.properties;
}

inline bool Amf0EcmaArray::operator==(const Amf0EcmaArray& other) const {
    if (properties == other.properties) return true;
    if (!properties || !other.properties) return false;
    return *properties == *other.properties;
}

inline bool Amf0StrictArray::operator==(const Amf0StrictArray& other) const {
    if (items == other.items) return true;
    if (!items || !other.items) return false;
    return *items == *other.items;
}

} // namespace rtmp_server::protocol::amf0
