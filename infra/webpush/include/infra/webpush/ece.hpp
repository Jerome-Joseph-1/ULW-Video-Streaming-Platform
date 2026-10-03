#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

// Message encryption for Web Push (RFC 8291): the "aes128gcm" content coding (RFC 8188) of one
// record, keyed by an ECDH between a fresh P-256 key of the sender's and the subscription's
// p256dh key, mixed with the subscription's auth secret. Only the browser that holds the
// subscription's private key can read the message; the push service carries ciphertext.
namespace infra::webpush {

// An uncompressed P-256 point: 0x04 || x || y.
inline constexpr std::size_t kPublicKeyBytes = 65;
inline constexpr std::size_t kPrivateKeyBytes = 32;
inline constexpr std::size_t kAuthSecretBytes = 16;
inline constexpr std::size_t kSaltBytes = 16;
// The record size this side writes: the 4096 octets a push service must accept (RFC 8030
// section 7.2) is the whole body, so one record always holds it.
inline constexpr std::uint32_t kRecordSize = 4096;
// salt (16) || rs (4) || idlen (1) || keyid (the sender's public key, 65).
inline constexpr std::size_t kHeaderBytes = kSaltBytes + 4 + 1 + kPublicKeyBytes;
inline constexpr std::size_t kTagBytes = 16;
// RFC 8291 section 4: 4096 less the header, the AEAD tag and the one-octet padding delimiter.
inline constexpr std::size_t kMaxPlaintext = 4096 - kHeaderBytes - kTagBytes - 1;

using PublicKey = std::array<std::uint8_t, kPublicKeyBytes>;
using PrivateKey = std::array<std::uint8_t, kPrivateKeyBytes>;
using AuthSecret = std::array<std::uint8_t, kAuthSecretBytes>;
using Salt = std::array<std::uint8_t, kSaltBytes>;

struct KeyPair {
    PrivateKey private_key;
    PublicKey public_key;
};

enum class EceError : std::uint8_t {
    // A public key that is not a point on P-256, or a private key out of range.
    BadKey,
    // More plaintext than one 4096-octet message holds.
    TooLarge,
    // Not a message this coding reads: too short, another record size or key length, a tag that
    // does not verify, or no padding delimiter.
    Malformed,
    // OpenSSL failed: out of memory, or no randomness.
    Crypto,
};

// Whether `bytes` is an uncompressed point on P-256: what a subscription's p256dh must be.
[[nodiscard]] bool is_p256_point(std::span<const std::uint8_t> bytes) noexcept;

// A fresh P-256 key pair.
[[nodiscard]] std::expected<KeyPair, EceError> generate_key_pair() noexcept;

// The body of a push message to the subscription (`ua_public`, `auth`): a fresh sender key and
// salt every call, as RFC 8291 requires, the plaintext in one record. With `pad_to`, the record
// (plaintext and delimiter) is padded with zeros to the next multiple of it, at most what one
// message holds, so that its length says less of what it carries (RFC 8291 section 4); 0 pads
// nothing past the delimiter.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, EceError>
encrypt(const PublicKey& ua_public, const AuthSecret& auth, std::span<const std::uint8_t> plaintext,
        std::size_t pad_to = 0) noexcept;

// encrypt() with the sender's key and the salt given: for the RFC's test vector, which a
// message must reproduce exactly. Never reuse either in a real message.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, EceError>
encrypt_with(const PrivateKey& as_private, const Salt& salt, const PublicKey& ua_public,
             const AuthSecret& auth, std::span<const std::uint8_t> plaintext,
             std::size_t pad_to = 0) noexcept;

// What the browser does: the plaintext of a message encrypted to (ua_private's public key,
// auth). One record, as this side writes them. For tests and the test push service.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, EceError>
decrypt(const PrivateKey& ua_private, const AuthSecret& auth,
        std::span<const std::uint8_t> message) noexcept;

} // namespace infra::webpush
