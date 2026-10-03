#pragma once

#include "core/models/ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The byte layouts the call handler's asks, answers and notices travel in between nodes
// (call.hpp, ring.hpp): integers big-endian, a short text behind a length byte, a long one
// behind two, and a UUID as its 36 characters.
namespace chat::layout {

inline constexpr std::size_t kUuidBytes = core::Uuid::kTextLength;

inline void put_u8(std::vector<std::byte>& out, std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

inline void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

inline void put_text(std::vector<std::byte>& out, std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text});
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// A user id: at most core::UserId::kMaxLength, which a length byte holds.
inline void put_short(std::vector<std::byte>& out, std::string_view text) {
    put_u8(out, static_cast<std::uint8_t>(text.size()));
    put_text(out, text);
}

// Endpoints and credentials are a URL and a JWT: well under 64 KiB.
inline void put_long(std::vector<std::byte>& out, std::string_view text) {
    put_u8(out, static_cast<std::uint8_t>((text.size() >> 8U) & 0xFFU));
    put_u8(out, static_cast<std::uint8_t>(text.size() & 0xFFU));
    put_text(out, text);
}

template <class Id> void put_uuid(std::vector<std::byte>& out, const Id& id) {
    std::array<char, kUuidBytes> text{};
    id.format_to(text);
    put_text(out, {text.data(), text.size()});
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : rest_(bytes) {}

    [[nodiscard]] std::optional<std::uint8_t> u8() noexcept {
        if (rest_.empty()) {
            return std::nullopt;
        }
        const auto v = std::to_integer<std::uint8_t>(rest_.front());
        rest_ = rest_.subspan(1);
        return v;
    }

    [[nodiscard]] std::optional<std::uint64_t> u64() noexcept {
        if (rest_.size() < sizeof(std::uint64_t)) {
            return std::nullopt;
        }
        std::uint64_t v = 0;
        for (const std::byte b : rest_.first(sizeof(std::uint64_t))) {
            v = (v << 8U) | std::to_integer<std::uint64_t>(b);
        }
        rest_ = rest_.subspan(sizeof(std::uint64_t));
        return v;
    }

    [[nodiscard]] std::optional<std::string_view> text(std::size_t n) noexcept {
        if (rest_.size() < n) {
            return std::nullopt;
        }
        // The bytes are characters; reading them as such is what this layout means.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const std::string_view out{reinterpret_cast<const char*>(rest_.data()), n};
        rest_ = rest_.subspan(n);
        return out;
    }

    [[nodiscard]] std::optional<std::string_view> short_text() noexcept {
        return u8().and_then([this](std::uint8_t n) { return text(n); });
    }

    [[nodiscard]] std::optional<std::string_view> long_text() noexcept {
        const auto high = u8();
        const auto low = u8();
        if (!high || !low) {
            return std::nullopt;
        }
        return text((std::size_t{*high} << 8U) | *low);
    }

    [[nodiscard]] std::optional<core::UserId> user() noexcept {
        const auto t = short_text();
        if (!t) {
            return std::nullopt;
        }
        auto parsed = core::UserId::parse(*t);
        if (!parsed) {
            return std::nullopt;
        }
        return *parsed;
    }

    template <class Id> [[nodiscard]] std::optional<Id> uuid() noexcept {
        const auto t = text(kUuidBytes);
        if (!t) {
            return std::nullopt;
        }
        auto parsed = Id::parse(*t);
        if (!parsed) {
            return std::nullopt;
        }
        return *parsed;
    }

    [[nodiscard]] bool empty() const noexcept { return rest_.empty(); }

private:
    std::span<const std::byte> rest_;
};

} // namespace chat::layout
