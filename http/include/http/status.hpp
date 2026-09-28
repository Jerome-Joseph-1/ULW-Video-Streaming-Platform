#pragma once

#include <cstdint>

namespace http {

enum class Status : std::uint16_t {
    BadRequest = 400,
    NotFound = 404,
    MethodNotAllowed = 405,
    LengthRequired = 411,
    ContentTooLarge = 413,
    RequestHeaderFieldsTooLarge = 431,
    NotImplemented = 501,
    HttpVersionNotSupported = 505,
};

[[nodiscard]] constexpr std::uint16_t code(Status s) noexcept {
    return static_cast<std::uint16_t>(s);
}

} // namespace http
