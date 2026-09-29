#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace infra::auth {

// RFC 4648 section 5 without padding, which is how JOSE writes every binary value.
[[nodiscard]] std::string encode_base64url(std::span<const unsigned char> bytes);
[[nodiscard]] std::string encode_base64url(std::string_view bytes);

// Strict: the URL-safe alphabet only, no padding, and zero bits after the last whole byte.
// Accepting either of the latter would give one token several spellings.
[[nodiscard]] std::optional<std::string> decode_base64url(std::string_view text);

} // namespace infra::auth
