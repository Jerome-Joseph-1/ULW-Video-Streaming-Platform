#pragma once

#include "core/util/json.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace infra::auth {

// Which tokens are the operator's own backend rather than a user (ADR-0097): those whose claim
// ULW_SERVICE_CLAIM names (`scope` by default) holds the value ULW_SERVICE_SCOPE gives, as a
// token from the identity provider's client-credentials grant does. Shared by every service
// that takes service calls, under the same two settings.
struct ServiceClaim {
    std::string claim = "scope";
    // Empty: no token is a service's, whatever it holds.
    std::string value;
};

struct ServiceClaimRefusal {
    std::string variable;
    std::string reason;
};

// Reads ULW_SERVICE_CLAIM and ULW_SERVICE_SCOPE, each nullopt when unset or empty. The claim
// is 1 to 64 of A-Z a-z 0-9 _ . : / -, and none of the registered claims that every token
// carries with another meaning (iss, aud, exp, nbf, iat, jti); the value 1 to 128 printable
// ASCII characters without spaces, since a scope claim separates its values with them. A claim
// without a value is refused: a deployment that names one means to take service calls.
[[nodiscard]] std::expected<ServiceClaim, ServiceClaimRefusal>
read_service_claim(std::optional<std::string_view> claim, std::optional<std::string_view> scope);

// Whether a claim's value holds `value`: a string equal to it, or listing it among values
// separated by spaces (OAuth's `scope`, RFC 8693 section 4.2); an array with a string that
// does either; or `true` when `value` is "true". Any other shape holds nothing.
[[nodiscard]] bool claim_holds(const core::json::Value& claim, std::string_view value) noexcept;

} // namespace infra::auth
