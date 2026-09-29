#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>

namespace rt {

// The id a client gives each message it sends, so that sending it again (after `unavailable`,
// which leaves its fate unknown) is recognised as the same message. Chosen by the client and
// unique per sender and room; the room plane never interprets it beyond comparing.
class MessageKey {
public:
    // A UUID in text is 36 characters, a ULID 26; 64 leaves room for a client's own scheme.
    static constexpr std::size_t kMaxLength = 64;

    // [A-Za-z0-9_-]: what random ids are written in, and nothing that needs escaping in JSON.
    [[nodiscard]] static std::optional<MessageKey> parse(std::string_view text) noexcept {
        const auto allowed = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '-';
        };
        if (text.empty() || text.size() > kMaxLength || !std::ranges::all_of(text, allowed)) {
            return std::nullopt;
        }
        MessageKey key;
        std::ranges::copy(text, key.chars_.begin());
        key.size_ = static_cast<std::uint8_t>(text.size());
        return key;
    }

    [[nodiscard]] std::string_view view() const noexcept { return {chars_.data(), size_}; }

    friend bool operator==(const MessageKey& a, const MessageKey& b) noexcept {
        return a.view() == b.view();
    }

private:
    MessageKey() = default;

    std::array<char, kMaxLength> chars_{};
    std::uint8_t size_ = 0;
    static_assert(kMaxLength <= std::numeric_limits<std::uint8_t>::max());
};

} // namespace rt

template <> struct std::hash<rt::MessageKey> {
    [[nodiscard]] std::size_t operator()(const rt::MessageKey& key) const noexcept {
        return std::hash<std::string_view>{}(key.view());
    }
};
