#include "presence_room.hpp"

#include "sha256.hpp"

#include <cstddef>
#include <format>
#include <iterator>
#include <span>
#include <string>

namespace chat {

namespace {

// Ahead of the user id in what is hashed, so that the same user id hashed for some other
// purpose never names this room.
constexpr std::string_view kNamespace = "ulw presence room\n";

} // namespace

core::RoomId presence_room(const core::UserId& user) {
    std::string name(kNamespace);
    name += user.view();
    auto digest = sha256(name);
    // The first 16 bytes, with the version and variant bits set (RFC 9562 section 5.8).
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0FU) | 0x80U);
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3FU) | 0x80U);
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

} // namespace chat
