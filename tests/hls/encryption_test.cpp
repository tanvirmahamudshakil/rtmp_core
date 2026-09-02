#include <gtest/gtest.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <span>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/hls/encryption.hpp"

using namespace rtmp_server;
using namespace rtmp_server::hls;

namespace {

// Decrypts with OpenSSL directly rather than with a helper from the same
// translation unit as the encryptor: a round trip through our own code would
// still pass if both halves shared the same wrong mode or padding, and a
// player only ever uses the standard primitive.
std::vector<std::byte> aes_128_cbc_decrypt(std::span<const std::byte> cipher,
                                           std::span<const std::byte> key,
                                           std::span<const std::byte> iv) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EXPECT_NE(ctx, nullptr);
    EXPECT_EQ(EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                                 reinterpret_cast<const unsigned char*>(key.data()),
                                 reinterpret_cast<const unsigned char*>(iv.data())),
              1);
    std::vector<std::byte> out(cipher.size() + 16);
    int written = 0;
    EXPECT_EQ(EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &written,
                                reinterpret_cast<const unsigned char*>(cipher.data()),
                                static_cast<int>(cipher.size())),
              1);
    int final_written = 0;
    EXPECT_EQ(EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + written,
                                  &final_written),
              1);
    EVP_CIPHER_CTX_free(ctx);
    out.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(final_written));
    return out;
}

std::vector<std::byte> synthetic_segment(std::size_t size) {
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::byte>((i * 7 + 13) & 0xFF);
    return data;
}

EncryptionConfig enabled_config() {
    EncryptionConfig config;
    config.enabled = true;
    config.key_uri_template = "key-{kid}.bin";
    return config;
}

} // namespace

TEST(EncryptionTest, DisabledEncryptorPassesMediaThroughUnchanged) {
    SegmentEncryptor encryptor({});
    EXPECT_FALSE(encryptor.enabled());

    const auto plain = synthetic_segment(1000);
    const auto result = encryptor.encrypt_segment(plain, 7);
    ASSERT_TRUE(result.ok());
    const auto view = result.value().view();
    ASSERT_EQ(view.size(), plain.size());
    EXPECT_TRUE(std::equal(view.begin(), view.end(), plain.begin()));
    EXPECT_EQ(encryptor.key_info(7), nullptr);
}

TEST(EncryptionTest, SegmentRoundTripsThroughAStandardAesDecrypt) {
    SegmentEncryptor encryptor(enabled_config());
    const auto plain = synthetic_segment(1880); // 10 TS packets

    const auto encrypted = encryptor.encrypt_segment(plain, 42);
    ASSERT_TRUE(encrypted.ok());
    const auto cipher = encrypted.value().view();
    // PKCS#7 pads up to the next whole block, always adding at least one byte
    // (so an already-aligned input grows by a full block).
    EXPECT_EQ(cipher.size(), (plain.size() / kAesBlockBytes + 1) * kAesBlockBytes);
    EXPECT_FALSE(std::equal(cipher.begin(), cipher.begin() + plain.size(), plain.begin()));

    const auto info = encryptor.key_info(42);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->encrypted());
    const std::string id = info->uri.substr(4, info->uri.size() - 4 - 4); // key-<id>.bin
    const auto key = encryptor.find_key(id);
    ASSERT_TRUE(key.has_value());

    const auto iv = iv_from_sequence(42);
    const auto decrypted = aes_128_cbc_decrypt(cipher, key->bytes, iv);
    EXPECT_EQ(decrypted, plain);
}

TEST(EncryptionTest, PartialSegmentUsesTheParentSegmentsKeyAndIv) {
    // RFC 8216bis 6.2.3: a part carries no IV of its own, so it must decrypt
    // under exactly what its segment's EXT-X-KEY advertises. If the encryptor
    // derived a per-part IV instead, a player would produce garbage.
    SegmentEncryptor encryptor(enabled_config());
    const auto part_plain = synthetic_segment(376);

    const auto encrypted = encryptor.encrypt_part(part_plain, 42);
    ASSERT_TRUE(encrypted.ok());

    const auto info = encryptor.key_info(42);
    ASSERT_NE(info, nullptr);
    const std::string id = info->uri.substr(4, info->uri.size() - 4 - 4);
    const auto key = encryptor.find_key(id);
    ASSERT_TRUE(key.has_value());

    const auto decrypted =
        aes_128_cbc_decrypt(encrypted.value().view(), key->bytes, iv_from_sequence(42));
    EXPECT_EQ(decrypted, part_plain);
}

TEST(EncryptionTest, IvIsTheBigEndianMediaSequenceNumber) {
    const auto iv = iv_from_sequence(0x0102030405060708ull);
    // The low 64 bits sit in the last eight bytes, most significant first;
    // everything above them is zero. This is the derivation RFC 8216 4.3.2.4
    // specifies for a playlist with no explicit IV, so a player that ignores
    // our explicit IV attribute still decrypts correctly.
    for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(static_cast<std::uint8_t>(iv[i]), 0u);
    EXPECT_EQ(static_cast<std::uint8_t>(iv[8]), 0x01u);
    EXPECT_EQ(static_cast<std::uint8_t>(iv[15]), 0x08u);
    EXPECT_EQ(iv_to_hex(iv), "0x00000000000000000102030405060708");
}

TEST(EncryptionTest, KeyInfoCarriesTheMethodUriAndPerSegmentIv) {
    auto config = enabled_config();
    config.key_format = "identity";
    SegmentEncryptor encryptor(config);

    const auto first = encryptor.key_info(1);
    const auto second = encryptor.key_info(2);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->method, "AES-128");
    EXPECT_EQ(first->key_format, "identity");
    // Same key while it is current, but each segment states its own IV.
    EXPECT_EQ(first->uri, second->uri);
    EXPECT_NE(first->iv_hex, second->iv_hex);
    EXPECT_EQ(first->iv_hex, iv_to_hex(iv_from_sequence(1)));
}

TEST(EncryptionTest, RotationMintsANewKeyAndRetiresOldOnesAfterTheRetentionBound) {
    auto config = enabled_config();
    config.retained_keys = 2;
    SegmentEncryptor encryptor(config);

    const auto first_uri = encryptor.key_info(1)->uri;
    const std::string first_id = first_uri.substr(4, first_uri.size() - 8);
    ASSERT_TRUE(encryptor.find_key(first_id).has_value());

    encryptor.rotate();
    const auto second_uri = encryptor.key_info(2)->uri;
    const std::string second_id = second_uri.substr(4, second_uri.size() - 8);
    EXPECT_NE(first_id, second_id);
    // The previous key stays fetchable: a player that read the playlist just
    // before the rotation is still holding a URI that names it.
    EXPECT_TRUE(encryptor.find_key(first_id).has_value());
    EXPECT_TRUE(encryptor.find_key(second_id).has_value());

    encryptor.rotate();
    const auto third_uri = encryptor.key_info(3)->uri;
    const std::string third_id = third_uri.substr(4, third_uri.size() - 8);
    // retained_keys = 2, so the oldest is now gone and its media is
    // permanently undecryptable — which is the point of rotating.
    EXPECT_FALSE(encryptor.find_key(first_id).has_value());
    EXPECT_TRUE(encryptor.find_key(second_id).has_value());
    EXPECT_TRUE(encryptor.find_key(third_id).has_value());
}

TEST(EncryptionTest, AnUnknownKeyIdIsNotSatisfiedWithAFreshKey) {
    SegmentEncryptor encryptor(enabled_config());
    (void)encryptor.key_info(1); // mint the current key
    // Minting on demand would let anyone with a playback token obtain a key
    // for media they never had a playlist for.
    EXPECT_FALSE(encryptor.find_key("deadbeefdeadbeefdeadbeefdeadbeef").has_value());
    EXPECT_FALSE(encryptor.find_key("").has_value());
}

TEST(EncryptionTest, RotationIntervalMintsANewKeyOnceItElapses) {
    auto config = enabled_config();
    config.rotation_interval = std::chrono::seconds(0); // never rotates on time
    SegmentEncryptor never(config);
    const auto a = never.key_info(1)->uri;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(never.key_info(2)->uri, a);

    // rotate() must still work when no interval is configured, so an operator
    // can revoke a leaked key immediately.
    never.rotate();
    EXPECT_NE(never.key_info(3)->uri, a);
}
