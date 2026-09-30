#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace chat {

// SHA-256 of `text`, for the names presence derives: a user's room and a node's tag.
[[nodiscard]] std::array<unsigned char, 32> sha256(std::string_view text);

} // namespace chat
