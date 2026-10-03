#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace infra::auth {

// RFC 4648 section 5 without padding, which is how JOSE writes every binary value.
[[nodiscard]] std::string encode_base64url(std::span<const unsigned char> bytes);
[[nodiscard]] std::string encode_base64url(std::string_view bytes);
// The same, appended to `out`, which grows once by the encoded length.
void append_base64url(std::string& out, std::span<const unsigned char> bytes);

// Strict: the URL-safe alphabet only, no padding, and zero bits after the last whole byte.
// Accepting either of the latter would give one token several spellings.
[[nodiscard]] std::optional<std::string> decode_base64url(std::string_view text);
// The same, into bytes, for callers that hold no text.
[[nodiscard]] std::optional<std::vector<std::byte>> decode_base64url_bytes(std::string_view text);

} // namespace infra::auth
