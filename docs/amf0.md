# AMF0

> Status: implemented in Phase 4. See `docs/phase4-checklist.md` for the
> phase tracking record.

## Scope

Action Message Format 0 (AMF0) — the value-serialization format RTMP uses
for command messages (`connect`, `createStream`, `publish`, ...) and
metadata (`@setDataFrame`, `onMetaData`). Implemented as a small,
allocation-conscious codec with no socket/io_uring dependency, consistent
with `protocol::handshake::HandshakeSession` and `protocol::chunk::
ChunkDecoder`/`ChunkEncoder` (docs/architecture.md "Architectural
Separation"):

* `rtmp_server::protocol::amf0::Amf0Value`
  (`include/rtmp_server/protocol/amf0/amf0_value.hpp`) — the value model.
* `encode()` (`include/rtmp_server/protocol/amf0/amf0_encoder.hpp`,
  `src/protocol/amf0/amf0_encoder.cpp`) — serializes an `Amf0Value` to bytes.
* `decode()` / `decode_all()`
  (`include/rtmp_server/protocol/amf0/amf0_decoder.hpp`,
  `src/protocol/amf0/amf0_decoder.cpp`) — parses bytes back into
  `Amf0Value`s, cursor-based (reports bytes consumed) like
  `ChunkDecoder::decode_one`.

AMF3 (message type 17) is out of scope — not required by the phase spec,
and no client this server targets (OBS, ffmpeg, standard RTMP publishers)
sends it for basic connect/publish/play flows; `objectEncoding` is always
advertised as `0` (AMF0) in the `connect` `_result`.

## Value model

`Amf0Value` (`amf0_value.hpp`) is a tagged union (`std::variant` under the
hood) over exactly the AMF0 types this server needs:

| AMF0 type    | Marker | `Amf0Value` factory                    | Accessor                          |
|--------------|--------|------------------------------------------|-------------------------------------|
| Number       | 0x00   | `Amf0Value::number(double)`              | `as_number()`                       |
| Boolean      | 0x01   | `Amf0Value::boolean(bool)`               | `as_boolean()`                      |
| String       | 0x02   | `Amf0Value::string(std::string)`         | `as_string()`                       |
| Object       | 0x03   | `Amf0Value::object(Amf0PropertyList)`    | `as_object()`, `find(key)`          |
| Null         | 0x05   | `Amf0Value::null()`                      | `is_null()`                         |
| Undefined    | 0x06   | `Amf0Value::undefined()`                 | `is_undefined()`                    |
| ECMA Array   | 0x08   | `Amf0Value::ecma_array(Amf0PropertyList)`| `as_ecma_array()`, `find(key)`      |
| Strict Array | 0x0A   | `Amf0Value::strict_array(Amf0ValueList)` | `as_strict_array()`                 |
| Date         | 0x0B   | `Amf0Value::date(double ms, int16 tz)`   | `as_date()`                         |
| Long String  | 0x0C   | *(same as String)*                       | `as_string()`                       |

Long String is **not** a distinct value in the model — it is purely a wire
encoding: `encode()` picks the `0x02 String` marker for strings up to 65535
bytes and `0x0C LongString` for anything longer; both decode back into a
plain `std::string`. `Object` and `EcmaArray` are distinct `std::variant`
alternatives (not a shared payload type with a tag byte) so the variant's
own discriminant recovers the wire type without extra bookkeeping.

`Object`/`EcmaArray`/`StrictArray` payloads (`Amf0PropertyList` /
`Amf0ValueList`) are recursive — an Object's values can themselves be
Objects. This is handled with a `std::shared_ptr<const T>` indirection
inside those three wrapper structs, **not** a directly-nested
`std::vector<std::pair<std::string, Amf0Value>>` member. That distinction
matters: `std::vector`/`list`/`forward_list` are permitted to hold an
incomplete element type at the point they're declared (P0307), but
`std::pair` is not guaranteed to be — and the *first* implicit
instantiation of `vector<pair<string, Amf0Value>>` inside `amf0_value.hpp`
happens while `Amf0Value` is still being defined (incomplete), which
poisons that template instantiation for the rest of the translation unit
under the one-definition rule and breaks completely unrelated call sites
(this was hit and fixed during Phase 4 development — see the comment at
the top of `amf0_value.hpp`). `shared_ptr<T>`'s layout never depends on
`T`, and its deleter is only instantiated at the construction sites in
`amf0_value.hpp`'s factory functions (written after `Amf0Value` is
complete), so it sidesteps the problem entirely. Values are treated as
immutable once constructed, so sharing the pointee across `Amf0Value`
copies (rather than deep-copying) is safe and cheap.

## Wire format

Every AMF0 value starts with a 1-byte type marker, followed by a
type-specific body:

```text
Number:      marker(1) value(8, IEEE-754 double, big-endian)
Boolean:     marker(1) value(1, 0x00 = false, nonzero = true)
String:      marker(1) length(2, u16 BE) utf8-bytes(length)
Long String: marker(1) length(4, u32 BE) utf8-bytes(length)
Null:        marker(1)
Undefined:   marker(1)
Date:        marker(1) millis-since-epoch(8, double BE) timezone(2, i16 BE)
Strict Array: marker(1) count(4, u32 BE) value*count
Object:      marker(1) property-list
ECMA Array:  marker(1) associative-count(4, u32 BE, hint only) property-list
```

`property-list` (shared by Object and ECMA Array) is a run of
`(name-length(2, u16 BE), name-bytes, value)` triples terminated by an
empty name (`u16 = 0`) immediately followed by the reserved
Object-End marker (`0x09`):

```text
property-list := ( u16-length name-bytes value )* u16(0) 0x09
```

Property names always use the short (u16-length) form on the wire, even
inside a Long-String-eligible value — only top-level String/LongString
values pick their marker based on length.

ECMA Array's leading 4-byte count is a hint some encoders get wrong or
omit meaningfully; this decoder reads and discards it, relying entirely on
the property-list terminator to know when the array ends (matching how
permissive real-world RTMP servers behave, since a wrong count must never
cause either a short read or a runaway parse).

## Decoding

`decode(std::span<const std::byte> data)` parses exactly one value
starting at `data[0]` and returns `Amf0Decoded{value, bytes_consumed}` via
`core::Result` — no exceptions, matching `ChunkDecoder`'s error-handling
convention (`core::ErrorCode::MalformedAmf` on any truncation, unknown
marker, or malformed Object/ECMA-Array terminator). `decode_all(data)`
repeatedly calls `decode()` until the whole span is consumed, which is
exactly the shape of one AMF0 command/data RTMP message payload (command
name, transaction ID, command object, then zero or more arguments, with no
outer envelope or count prefix).

Recursive structures (Object/ECMA-Array property values, Strict-Array
items) are decoded by direct recursion into `decode_value()`; a
`kMaxProperties` sanity cap (1,000,000) on property-list length exists as
defense-in-depth against a pathological "terminator never appears" input,
though `require()`'s bounds check against the actual buffer length is what
primarily prevents unbounded parsing of finite input.

## Encoding

`encode(const Amf0Value&, std::vector<std::byte>& out)` appends one value's
wire encoding to `out`; encoding a whole command message is just calling
this once per value into the same buffer, in order. `encode(const
Amf0Value&) -> std::vector<std::byte>` is a convenience overload for a
single value.

## Known limitations

* AMF3 (Action Message Format 3, message type 17 "AMF3 Command") is not
  implemented — out of scope per the phase spec. A client that negotiates
  `objectEncoding: 3` and sends AMF3 command messages is not supported;
  this server always advertises AMF0 (`objectEncoding: 0`) in its
  `connect` `_result` (see `docs/rtmp-commands.md`).
* `MovieClip` (0x04), `Reference` (0x07), `Unsupported` (0x0D),
  `RecordSet` (0x0E), `XML Document` (0x0F), and `Typed Object` (0x10)
  markers are recognized (named in `Amf0Marker`) but rejected as malformed
  on decode and cannot be produced by the encoder — none of them appear in
  RTMP connect/createStream/publish/play/deleteStream traffic from any
  mainstream publisher or player.
* No fuzz target yet for `decode`/`decode_all` (same deferral as
  `ChunkDecoder`, see `docs/chunk-parser.md` "Known limitations" — a later,
  cross-cutting phase).
