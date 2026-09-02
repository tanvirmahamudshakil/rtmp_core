#include "rtmp_server/hls/encryption.hpp"

#include <algorithm>
#include <openssl/evp.h>

#include "rtmp_server/core/random.hpp"

namespace rtmp_server::hls {

namespace {

core::Error crypto_failure(std::string_view what) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, what);
}

std::string replace_all(std::string subject, std::string_view needle, std::string_view value) {
    std::size_t position = 0;
    while ((position = subject.find(needle, position)) != std::string::npos) {
        subject.replace(position, needle.size(), value);
        position += value.size();
    }
    return subject;
}

// One AES-128-CBC pass with PKCS#7 padding. A fresh context per call: these
// run on the media thread once per segment or part, so context reuse would
// save nothing measurable and would make the encryptor stateful across
// threads for no reason.
core::Result<std::vector<std::byte>> aes_128_cbc_encrypt(std::span<const std::byte> plain,
                                                         std::span<const std::byte> key,
                                                         std::span<const std::byte> iv) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) return crypto_failure("EVP_CIPHER_CTX_new failed");

    struct ContextGuard {
        EVP_CIPHER_CTX* ctx;
        ~ContextGuard() { EVP_CIPHER_CTX_free(ctx); }
    } guard{ctx};

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(iv.data())) != 1) {
        return crypto_failure("EVP_EncryptInit_ex failed");
    }

    // PKCS#7 padding adds between 1 and one full block, so the ciphertext is
    // never longer than this.
    std::vector<std::byte> out(plain.size() + kAesBlockBytes);
    int written = 0;
    if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &written,
                          reinterpret_cast<const unsigned char*>(plain.data()),
                          static_cast<int>(plain.size())) != 1) {
        return crypto_failure("EVP_EncryptUpdate failed");
    }
    int final_written = 0;
    if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + written,
                            &final_written) != 1) {
        return crypto_failure("EVP_EncryptFinal_ex failed");
    }
    out.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(final_written));
    return out;
}

} // namespace

std::array<std::byte, kAesBlockBytes> iv_from_sequence(std::uint64_t sequence) {
    std::array<std::byte, kAesBlockBytes> iv{};
    for (std::size_t i = 0; i < 8; ++i) {
        iv[kAesBlockBytes - 1 - i] = static_cast<std::byte>((sequence >> (8 * i)) & 0xFFu);
    }
    return iv;
}

std::string iv_to_hex(std::span<const std::byte> iv) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "0x";
    out.reserve(2 + iv.size() * 2);
    for (const std::byte b : iv) {
        const auto value = static_cast<std::uint8_t>(b);
        out.push_back(kHex[value >> 4]);
        out.push_back(kHex[value & 0x0F]);
    }
    return out;
}

SegmentEncryptor::SegmentEncryptor(EncryptionConfig config) : config_(std::move(config)) {
    if (config_.retained_keys == 0) config_.retained_keys = 1;
}

const ContentKey& SegmentEncryptor::current_key_locked() {
    const auto now = std::chrono::steady_clock::now();
    const bool due = config_.rotation_interval.count() > 0 &&
                     now - key_minted_at_ >= config_.rotation_interval;
    if (keys_.empty() || due) {
        ContentKey key;
        // 128 bits of id: the id travels in a public playlist, so it must not
        // be guessable — a predictable id would let anyone enumerate keys
        // through the delivery endpoint's authorisation window.
        key.id = core::generate_secure_token(16);
        core::secure_random_bytes(key.bytes);
        keys_.push_back(std::move(key));
        key_minted_at_ = now;
        while (keys_.size() > config_.retained_keys) keys_.pop_front();
    }
    return keys_.back();
}

core::Result<core::SharedBuffer> SegmentEncryptor::encrypt_segment(std::span<const std::byte> plain,
                                                                    std::uint64_t sequence) {
    if (!config_.enabled) return core::SharedBuffer::copy_from(plain);

    std::array<std::byte, kAesKeyBytes> key{};
    {
        std::lock_guard lock(mutex_);
        key = current_key_locked().bytes;
    }
    // Encryption itself runs outside the lock: it is the expensive part, and
    // holding the mutex across it would serialise every stream sharing this
    // encryptor against one another.
    const auto iv = iv_from_sequence(sequence);
    auto encrypted = aes_128_cbc_encrypt(plain, key, iv);
    if (!encrypted.ok()) return encrypted.error();
    return core::SharedBuffer::adopt(std::move(encrypted).value());
}

core::Result<core::SharedBuffer> SegmentEncryptor::encrypt_part(std::span<const std::byte> plain,
                                                                 std::uint64_t segment_sequence) {
    // Same key, same IV as the parent segment (RFC 8216bis 6.2.3): a part is
    // its own CBC stream, so it decrypts standalone, and a player that has
    // the segment's EXT-X-KEY already has everything it needs.
    return encrypt_segment(plain, segment_sequence);
}

EncryptionKeyInfoPtr SegmentEncryptor::key_info(std::uint64_t sequence) {
    if (!config_.enabled) return nullptr;

    std::string key_id;
    {
        std::lock_guard lock(mutex_);
        key_id = current_key_locked().id;
    }

    auto info = std::make_shared<EncryptionKeyInfo>();
    info->method = "AES-128";
    info->uri = replace_all(config_.key_uri_template, "{kid}", key_id);
    info->iv_hex = iv_to_hex(iv_from_sequence(sequence));
    info->key_format = config_.key_format;
    info->key_format_versions = config_.key_format_versions;
    return info;
}

std::optional<ContentKey> SegmentEncryptor::find_key(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(keys_.begin(), keys_.end(),
                                 [&id](const ContentKey& key) { return key.id == id; });
    if (it == keys_.end()) return std::nullopt;
    return *it;
}

void SegmentEncryptor::rotate() {
    std::lock_guard lock(mutex_);
    // Backdating the mint time makes the next current_key_locked() call mint
    // a replacement, keeping every rotation on one code path.
    if (config_.rotation_interval.count() > 0) {
        key_minted_at_ = std::chrono::steady_clock::now() - config_.rotation_interval;
        return;
    }
    ContentKey key;
    key.id = core::generate_secure_token(16);
    core::secure_random_bytes(key.bytes);
    keys_.push_back(std::move(key));
    key_minted_at_ = std::chrono::steady_clock::now();
    while (keys_.size() > config_.retained_keys) keys_.pop_front();
}

} // namespace rtmp_server::hls
