#pragma once

#include "core/ports/auth.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace infra::auth::detail {

// Browsers keep a cookie to 4096 bytes (RFC 6265 section 6.1), which bounds a token from the
// cookie; an RS256 token with ordinary claims is under 1 KiB. Twice the cookie limit leaves
// room for a bearer token with larger claims, at half the 16 KiB header budget of the parser.
inline constexpr std::size_t kMaxTokenBytes = std::size_t{8} * 1024;

// Kids in the wild are thumbprints (43 characters), UUIDs (36) or short names; 256 keeps a
// junk kid from costing more than a few hundred bytes wherever it is remembered.
inline constexpr std::size_t kMaxKidBytes = 256;

enum class Algorithm : std::uint8_t { RS256, PS256, ES256, EdDSA };

[[nodiscard]] std::optional<Algorithm> parse_algorithm(std::string_view name) noexcept;

// A compact JWS whose segments are well formed. Nothing in it is authenticated: the payload is
// decoded but must not be parsed until the signature over `signing_input` has been checked.
struct CompactJws {
    Algorithm alg;
    std::string kid;
    std::string_view signing_input;
    std::string payload;
    std::string signature;
};

// Rejects alg "none" and every algorithm outside the four above before any key is looked up,
// so such a token can never cause a key fetch. `signing_input` views `token`.
[[nodiscard]] std::expected<CompactJws, core::ports::AuthError>
parse_compact(std::string_view token);
// A temporary token would leave `signing_input` dangling.
std::expected<CompactJws, core::ports::AuthError> parse_compact(std::string&& token) = delete;

} // namespace infra::auth::detail
