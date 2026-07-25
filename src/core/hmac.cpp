#include "rtmp_server/core/hmac.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <stdexcept>

namespace rtmp_server::core {

namespace {

std::string to_hex(const unsigned char* bytes, unsigned int length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<std::size_t>(length) * 2);
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

} // namespace

std::string hmac_sha256_hex(std::string_view secret, std::string_view message) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;

    const unsigned char* result =
        HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest.data(), &digest_len);
    if (result == nullptr) {
        // Same posture as secure_random_bytes: an OpenSSL primitive failing
        // is a fatal, not recoverable, condition — every signed token's
        // integrity depends on it.
        throw std::runtime_error("hmac_sha256_hex: HMAC failed");
    }
    return to_hex(digest.data(), digest_len);
}

std::string sha256_hex(std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) throw std::runtime_error("sha256_hex: EVP_MD_CTX_new failed");

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) == 1;
    EVP_MD_CTX_free(ctx);

    if (!ok) throw std::runtime_error("sha256_hex: digest failed");
    return to_hex(digest.data(), digest_len);
}

bool constant_time_equals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace rtmp_server::core
