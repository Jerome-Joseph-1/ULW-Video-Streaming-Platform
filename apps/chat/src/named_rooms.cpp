#include "named_rooms.hpp"

#include "core/ports/message_store.hpp"

#include "sha256.hpp"

#include <cstddef>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

namespace chat {

namespace {

// RFC 9562 section 4: the version is the high nibble of byte 6, the variant the top two bits of
// byte 8.
constexpr std::size_t kVersionByte = 6;
constexpr std::size_t kVariantByte = 8;

// The first 16 bytes of SHA-256 over `name`, tagged and with the version and variant bits set.
core::RoomId named(core::ports::NamedRoom tag, std::string_view name) {
    auto digest = sha256(name);
    digest[0] = static_cast<unsigned char>(tag);
    digest[kVersionByte] = static_cast<unsigned char>((digest[kVersionByte] & 0x0FU) | 0x80U);
    digest[kVariantByte] = static_cast<unsigned char>((digest[kVariantByte] & 0x3FU) | 0x80U);
    std::string text;
    std::size_t at = 0;
    for (const unsigned char b : std::span(digest).first<core::Uuid::kByteLength>()) {
        if (at == 4 || at == 6 || at == 8 || at == 10) {
            text += '-';
        }
        std::format_to(std::back_inserter(text), "{:02x}", b);
        ++at;
    }
    return *core::RoomId::parse(text);
}

} // namespace

// Ids hold no newline (core::UserId, rt::MessageKey), so the fields cannot run into each other,
// and the namespace line keeps these names apart from every other hash of the same ids.
core::RoomId direct_room(const core::UserId& a, const core::UserId& b) {
    const bool ordered = a.view() <= b.view();
    const std::string_view first = ordered ? a.view() : b.view();
    const std::string_view second = ordered ? b.view() : a.view();
    std::string name = "ulw direct chat\n";
    name += first;
    name += '\n';
    name += second;
    return named(core::ports::NamedRoom::DirectChat, name);
}

core::RoomId group_room(const core::UserId& creator, const rt::MessageKey& request) {
    std::string name = "ulw group chat\n";
    name += creator.view();
    name += '\n';
    name += request.view();
    return named(core::ports::NamedRoom::GroupChat, name);
}

} // namespace chat
