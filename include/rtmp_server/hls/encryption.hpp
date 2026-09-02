#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rtmp_server/core/buffer.hpp"
#include "rtmp_server/core/result.hpp"
#include "rtmp_server/hls/segment.hpp"

// AES-128 HLS segment encryption (RFC 8216 4.3.2.4 / 6.2.3), the missing
// half of this server's playback access control: signed URL tokens keep an
// unauthorised player from *fetching* media, but anything that does obtain a
// segment URL gets plaintext. With encryption on, a segment is useless
// without a key fetched from an endpoint that enforces the same
// authorisation, and rotating the key bounds how long a leaked one is worth.
//
// Whole-segment AES-128-CBC with PKCS#7 padding, which is what every HLS
// player implements. SAMPLE-AES is deliberately not offered: it requires
// codec-aware partial encryption and, in practice, a DRM system's key
// delivery to be worth anything over this.
namespace rtmp_server::hls {

inline constexpr std::size_t kAesKeyBytes = 16;
inline constexpr std::size_t kAesBlockBytes = 16;

struct EncryptionConfig {
    bool enabled = false;

    // How long one key stays current. 0 never rotates. A rotation only
    // affects segments produced after it; segments already in the live
    // window keep naming the key that actually decrypts them.
    std::chrono::seconds rotation_interval{0};

    // Retired keys kept fetchable after a rotation. A player that read the
    // playlist just before a rotation may still request the previous key, so
    // this must comfortably exceed the live window plus retention grace.
    std::size_t retained_keys = 4;

    // Playlist EXT-X-KEY URI. "{kid}" is replaced with the key's id, which is
    // how the delivery endpoint knows which key to return. Relative URIs are
    // resolved by the player against the playlist, so the default keeps the
    // key request inside the same stream path (and therefore inside the same
    // authorisation gate).
    std::string key_uri_template = "key/{kid}.bin";

    // Optional KEYFORMAT / KEYFORMATVERSIONS attributes. Empty omits them,
    // which selects the standard "identity" format every player supports.
    std::string key_format;
    std::string key_format_versions;
};

// One 128-bit content key plus the id its delivery URI carries.
struct ContentKey {
    std::string id; // hex, unpredictable — never a counter
    std::array<std::byte, kAesKeyBytes> bytes{};
};

// Owns the current key, mints rotations, and encrypts segment and partial
// segment payloads. Thread-safe: the media thread encrypts while HTTP worker
// threads resolve key ids for the delivery endpoint.
class SegmentEncryptor {
public:
    explicit SegmentEncryptor(EncryptionConfig config);

    [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }

    // Encrypts one whole segment. The IV is the 128-bit big-endian media
    // sequence number, the derivation RFC 8216 defines when no explicit IV is
    // given — but it is also written into the playlist explicitly, so a
    // player never has to guess which sequence a segment had.
    [[nodiscard]] core::Result<core::SharedBuffer> encrypt_segment(std::span<const std::byte> plain,
                                                                   std::uint64_t sequence);

    // Encrypts one Low-Latency HLS partial segment. RFC 8216bis 6.2.3: a
    // partial segment uses its parent segment's key and IV, so this takes the
    // parent's sequence number and produces an independently decryptable
    // CBC stream under that same IV.
    [[nodiscard]] core::Result<core::SharedBuffer> encrypt_part(std::span<const std::byte> plain,
                                                                std::uint64_t segment_sequence);

    // EXT-X-KEY description for a segment with this sequence number,
    // including the explicit IV. Null when encryption is disabled.
    [[nodiscard]] EncryptionKeyInfoPtr key_info(std::uint64_t sequence);

    // Resolves a key id from a delivery request. Returns nullopt for an
    // unknown or already-retired id — the caller answers 404, never a
    // freshly minted key, or an attacker could mint keys at will.
    [[nodiscard]] std::optional<ContentKey> find_key(const std::string& id) const;

    // Forces a rotation now. Exposed for the management API and for tests;
    // ordinary rotation happens on the configured interval.
    void rotate();

private:
    // Caller must hold mutex_. Mints the first key, or a replacement once the
    // rotation interval has elapsed.
    const ContentKey& current_key_locked();

    EncryptionConfig config_;
    mutable std::mutex mutex_;
    std::deque<ContentKey> keys_; // newest last
    std::chrono::steady_clock::time_point key_minted_at_{};
};

// The 128-bit big-endian IV a media sequence number derives, and its playlist
// "0x..." rendering. Exposed for tests and for the DASH packager, which needs
// the identical value.
[[nodiscard]] std::array<std::byte, kAesBlockBytes> iv_from_sequence(std::uint64_t sequence);
[[nodiscard]] std::string iv_to_hex(std::span<const std::byte> iv);

} // namespace rtmp_server::hls
