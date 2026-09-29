#include "presence_room.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <openssl/evp.h>
#include <stdexcept>
#include <string>

namespace chat {

namespace {

// Ahead of the user id in what is hashed, so that the same user id hashed for some other
// purpose never names this room.
constexpr std::string_view kNamespace = "ulw presence room\n";
constexpr std::byte kVersion8{0x80};

} // namespace

core::RoomId presence_room(const core::UserId& user) {
    std::string name(kNamespace);
    name += user.view();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    std::size_t length = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, name.data(), name.size(), digest.data(),
                     &length) != 1) {
        // SHA-256 is compiled into every OpenSSL this builds against; only a broken library
        // gets here.
        throw std::runtime_error("SHA-256 unavailable");
    }
    // The first 16 bytes, with the version and variant bits set (RFC 9562 section 5.8).
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0FU) | 0x80U);
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3FU) | 0x80U);
    std::string text;
    for (std::size_t i = 0; i < core::Uuid::kByteLength; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            text += '-';
        }
        std::format_to(std::back_inserter(text), "{:02x}", digest[i]);
    }
    return *core::RoomId::parse(text);
}

bool is_presence_room(const core::RoomId& room) noexcept {
    return (room.uuid().bytes()[6] & std::byte{0xF0}) == kVersion8;
}

} // namespace chat
