#include "jws.hpp"

#include "core/ports/auth.hpp"
#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include "json_member.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace infra::auth::detail {

using core::ports::AuthError;

std::optional<Algorithm> parse_algorithm(std::string_view name) noexcept {
    if (name == "RS256") {
        return Algorithm::RS256;
    }
    if (name == "PS256") {
        return Algorithm::PS256;
    }
    if (name == "ES256") {
        return Algorithm::ES256;
    }
    if (name == "EdDSA") {
        return Algorithm::EdDSA;
    }
    return std::nullopt;
}

std::expected<CompactJws, AuthError> parse_compact(std::string_view token) {
    if (token.empty() || token.size() > kMaxTokenBytes) {
        return std::unexpected(AuthError::Malformed);
    }
    const std::size_t first = token.find('.');
    const std::size_t second = first == std::string_view::npos ? first : token.find('.', first + 1);
    if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos) {
        return std::unexpected(AuthError::Malformed);
    }
    const std::string_view header_text = token.substr(0, first);
    const std::string_view payload_text = token.substr(first + 1, second - first - 1);
    const std::string_view signature_text = token.substr(second + 1);

    const std::optional<std::string> header_json = decode_base64url(header_text);
    if (!header_json) {
        return std::unexpected(AuthError::Malformed);
    }
    const auto header = core::json::parse(*header_json);
    if (!header || header->as_object() == nullptr) {
        return std::unexpected(AuthError::Malformed);
    }
    const std::optional<std::string_view> alg = string_member(*header, "alg");
    if (!alg) {
        return std::unexpected(AuthError::Malformed);
    }
    const std::optional<Algorithm> algorithm = parse_algorithm(*alg);
    if (!algorithm) {
        return std::unexpected(AuthError::UnsupportedAlgorithm);
    }
    // RFC 7515 section 4.1.11: a critical extension this code does not implement (b64, for
    // one, changes what the signature covers) makes the token invalid.
    if (header->find("crit") != nullptr) {
        return std::unexpected(AuthError::Malformed);
    }
    const std::optional<std::string_view> kid = string_member(*header, "kid");
    if (!kid || kid->empty() || kid->size() > kMaxKidBytes) {
        return std::unexpected(AuthError::Malformed);
    }

    std::optional<std::string> payload = decode_base64url(payload_text);
    std::optional<std::string> signature = decode_base64url(signature_text);
    if (!payload || payload->empty() || !signature || signature->empty()) {
        return std::unexpected(AuthError::Malformed);
    }
    return CompactJws{
        .alg = *algorithm,
        .kid = std::string(*kid),
        .signing_input = token.substr(0, second),
        .payload = std::move(*payload),
        .signature = std::move(*signature),
    };
}

} // namespace infra::auth::detail
