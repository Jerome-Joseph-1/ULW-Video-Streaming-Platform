#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace infra::s3util {

using Sha256Digest = std::array<unsigned char, 32>;

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data) noexcept;
[[nodiscard]] Sha256Digest sha256(std::string_view data) noexcept;
[[nodiscard]] Sha256Digest hmac_sha256(std::span<const unsigned char> key,
                                       std::string_view data) noexcept;

// Lowercase, as SigV4 wants it everywhere.
[[nodiscard]] std::string to_hex(std::span<const unsigned char> bytes);

} // namespace infra::s3util
