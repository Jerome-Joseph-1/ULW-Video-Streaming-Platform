#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <openssl/evp.h>
#include <optional>
#include <span>

// P-256 keys, ECDH and HMAC-SHA-256 on OpenSSL 3's EVP interfaces, for the message encryption
// (RFC 8291) and the VAPID signature (RFC 8292). Nothing here logs or keeps key material beyond
// the EVP_PKEY it is asked to build.
namespace infra::webpush::detail {

inline constexpr std::size_t kPointBytes = 65;
inline constexpr std::size_t kScalarBytes = 32;
inline constexpr std::size_t kSha256Bytes = 32;

struct PkeyFree {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;

using Point = std::array<std::uint8_t, kPointBytes>;
using Scalar = std::array<std::uint8_t, kScalarBytes>;
using Digest = std::array<std::uint8_t, kSha256Bytes>;

// An uncompressed point (0x04 || x || y), checked to lie on the curve; null otherwise.
[[nodiscard]] Pkey import_public(std::span<const std::uint8_t> point) noexcept;
// A private scalar, 1 to n - 1, with its public point computed from it; null otherwise.
[[nodiscard]] Pkey import_private(const Scalar& scalar) noexcept;
// A fresh key pair from OpenSSL's generator; null when it fails.
[[nodiscard]] Pkey generate() noexcept;
// The key's public point, uncompressed.
[[nodiscard]] std::optional<Point> public_point(EVP_PKEY* key) noexcept;
// The key's private scalar, big-endian, left-padded to 32 bytes.
[[nodiscard]] std::optional<Scalar> private_scalar(EVP_PKEY* key) noexcept;
// The x coordinate of mine x peer's point.
[[nodiscard]] std::optional<Scalar> ecdh(EVP_PKEY* mine, EVP_PKEY* peer) noexcept;
// HMAC-SHA-256(key, data).
[[nodiscard]] std::optional<Digest> hmac_sha256(std::span<const std::uint8_t> key,
                                                std::span<const std::uint8_t> data) noexcept;

} // namespace infra::webpush::detail
