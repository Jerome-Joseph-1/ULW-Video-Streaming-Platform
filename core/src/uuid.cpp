#include "core/util/uuid.hpp"

#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "core/util/time.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace core {

namespace {

// Where each byte's two hex digits sit in the 8-4-4-4-12 text form.
constexpr std::array<std::size_t, Uuid::kByteLength> kDigitOffsets{0,  2,  4,  6,  9,  11, 14, 16,
                                                                   19, 21, 24, 26, 28, 30, 32, 34};
constexpr std::array<std::size_t, 4> kDashOffsets{8, 13, 18, 23};
constexpr std::string_view kHexDigits = "0123456789abcdef";

std::optional<std::byte> decode_hex_pair(char high, char low) noexcept {
    const std::size_t h = kHexDigits.find(high);
    const std::size_t l = kHexDigits.find(low);
    if (h == std::string_view::npos || l == std::string_view::npos) {
        return std::nullopt;
    }
    return static_cast<std::byte>((h << 4U) | l);
}

} // namespace

std::optional<Uuid> Uuid::parse(std::string_view text) noexcept {
    if (text.size() != kTextLength) {
        return std::nullopt;
    }
    for (const std::size_t at : kDashOffsets) {
        if (text[at] != '-') {
            return std::nullopt;
        }
    }
    Uuid id;
    for (auto&& [out, at] : std::views::zip(id.bytes_, kDigitOffsets)) {
        const std::optional<std::byte> value = decode_hex_pair(text[at], text[at + 1]);
        if (!value) {
            return std::nullopt;
        }
        out = *value;
    }
    return id;
}

Uuid Uuid::v7(const ports::IClock& clock, ports::IRandom& random) noexcept {
    const auto since_epoch =
        std::chrono::duration_cast<Millis>(clock.wall_now().time_since_epoch());
    // The field is unsigned; a clock set before 1970 would otherwise wrap to the far future.
    auto ms = static_cast<std::uint64_t>(std::max(since_epoch, Millis::zero()).count());

    Uuid id;
    const std::span<std::byte, kByteLength> bytes(id.bytes_);
    for (std::byte& b : bytes.first<6>() | std::views::reverse) {
        b = static_cast<std::byte>(ms & 0xFFU);
        ms >>= 8U;
    }
    random.fill(bytes.subspan<6>());
    // The version nibble and the two variant bits overwrite 6 of the 80 random bits, leaving
    // rand_a (12 bits) and rand_b (62 bits).
    bytes[6] = (bytes[6] & std::byte{0x0F}) | std::byte{0x70};
    bytes[8] = (bytes[8] & std::byte{0x3F}) | std::byte{0x80};
    return id;
}

void Uuid::format_to(std::span<char, kTextLength> out) const noexcept {
    for (const std::size_t at : kDashOffsets) {
        out[at] = '-';
    }
    for (const auto [b, at] : std::views::zip(bytes_, kDigitOffsets)) {
        const auto value = std::to_integer<std::size_t>(b);
        out[at] = kHexDigits[value >> 4U];
        out[at + 1] = kHexDigits[value & 0x0FU];
    }
}

std::string Uuid::to_string() const {
    std::string text(kTextLength, '\0');
    format_to(std::span<char, kTextLength>(text.data(), kTextLength));
    return text;
}

} // namespace core

std::size_t std::hash<core::Uuid>::operator()(const core::Uuid& id) const noexcept {
    const auto raw = std::bit_cast<std::array<char, core::Uuid::kByteLength>>(id);
    return std::hash<std::string_view>{}(std::string_view(raw.data(), raw.size()));
}
