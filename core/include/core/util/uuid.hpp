#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace core {

class Uuid {
public:
    static constexpr std::size_t kByteLength = 16;
    // 32 hex digits in 8-4-4-4-12 groups joined by 4 dashes.
    static constexpr std::size_t kTextLength = 36;

    // Only the canonical lowercase form. Ids end up in URLs, storage keys and cache keys, where a
    // second accepted spelling would give one entity two distinct names.
    [[nodiscard]] static std::optional<Uuid> parse(std::string_view text) noexcept;

    // RFC 9562 version 7: time-ordered, so freshly minted ids append to the right edge of a
    // B-tree index instead of scattering writes across it. Ids from the same millisecond are
    // unordered among themselves; nothing needs finer ordering, so there is no monotonic counter.
    [[nodiscard]] static Uuid v7(const ports::IClock& clock, ports::IRandom& random) noexcept;

    [[nodiscard]] std::span<const std::byte, kByteLength> bytes() const& noexcept { return bytes_; }
    void bytes() const&& = delete;

    void format_to(std::span<char, kTextLength> out) const noexcept;
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const Uuid&, const Uuid&) = default;
    friend auto operator<=>(const Uuid&, const Uuid&) = default;

private:
    Uuid() = default;

    std::array<std::byte, kByteLength> bytes_{};
};

} // namespace core

template <> struct std::hash<core::Uuid> {
    [[nodiscard]] std::size_t operator()(const core::Uuid& id) const noexcept;
};
