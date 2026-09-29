#include "live_chat.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <openssl/evp.h>
#include <span>
#include <string>

namespace chat {

namespace {

// RFC 9562 section 4: the version is the high nibble of byte 6, the variant the top two bits
// of byte 8.
constexpr std::size_t kVersionByte = 6;
constexpr std::size_t kVariantByte = 8;
constexpr unsigned kVersion8 = 0x80U;
constexpr unsigned kVariant = 0x80U;

} // namespace

bool is_stream_name(std::string_view text) noexcept {
    constexpr std::size_t kMaxLength = 64;
    const auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    };
    return !text.empty() && text.size() <= kMaxLength && std::ranges::all_of(text, allowed);
}

std::optional<core::RoomId> live_chat_room(std::string_view stream) {
    const std::string input = "ulw-live-chat:" + std::string(stream);
    std::array<unsigned char, 32> digest{};
    std::size_t written = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, input.data(), input.size(), digest.data(),
                     &written) == 0 ||
        written != digest.size()) {
        return std::nullopt;
    }
    digest[kVersionByte] = static_cast<unsigned char>((digest[kVersionByte] & 0x0FU) | kVersion8);
    digest[kVariantByte] = static_cast<unsigned char>((digest[kVariantByte] & 0x3FU) | kVariant);
    std::string text;
    std::size_t at = 0;
    for (const unsigned char b : std::span(digest).first<core::Uuid::kByteLength>()) {
        if (at == 4 || at == 6 || at == 8 || at == 10) {
            text += '-';
        }
        text += std::format("{:02x}", b);
        ++at;
    }
    const auto room = core::RoomId::parse(text);
    if (!room) {
        return std::nullopt;
    }
    return *room;
}

} // namespace chat
