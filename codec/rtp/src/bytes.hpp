#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Network byte order reads at an offset the caller has already bounds-checked.
namespace codec::rtp::detail {

[[nodiscard]] inline std::uint8_t u8(std::span<const std::byte> b, std::size_t at) noexcept {
    return std::to_integer<std::uint8_t>(b[at]);
}

[[nodiscard]] inline std::uint16_t be16(std::span<const std::byte> b, std::size_t at) noexcept {
    return static_cast<std::uint16_t>((unsigned{u8(b, at)} << 8U) | u8(b, at + 1));
}

[[nodiscard]] inline std::uint32_t be24(std::span<const std::byte> b, std::size_t at) noexcept {
    return (std::uint32_t{u8(b, at)} << 16U) | (std::uint32_t{u8(b, at + 1)} << 8U) | u8(b, at + 2);
}

[[nodiscard]] inline std::uint32_t be32(std::span<const std::byte> b, std::size_t at) noexcept {
    return (std::uint32_t{be16(b, at)} << 16U) | be16(b, at + 2);
}

[[nodiscard]] inline std::uint64_t be64(std::span<const std::byte> b, std::size_t at) noexcept {
    return (std::uint64_t{be32(b, at)} << 32U) | be32(b, at + 4);
}

// A 24-bit two's complement field.
[[nodiscard]] inline std::int32_t signed24(std::uint32_t v) noexcept {
    constexpr std::uint32_t kSign = 0x80'0000;
    constexpr std::int32_t kRange = 0x100'0000;
    return (v & kSign) != 0 ? static_cast<std::int32_t>(v) - kRange : static_cast<std::int32_t>(v);
}

} // namespace codec::rtp::detail
