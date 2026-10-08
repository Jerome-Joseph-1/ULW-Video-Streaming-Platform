#pragma once

#include "core/util/json.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace infra::auth {

// Which tokens are the operator's own backend rather than a user (ADR-0096): those whose claim
// ULW_SERVICE_CLAIM names (`scope` by default) holds the value ULW_SERVICE_SCOPE gives, as a
// token from the identity provider's client-credentials grant does. Shared by every service
// that takes service calls, under the same two settings.
struct ServiceClaim {
    std::string claim = "scope";
    // Empty: no token is a service's, whatever it holds.
    std::string value;
    // ULW_SERVICE_CLIENT_ID: when set, a service token must also name this client in `azp` or
    // `client_id`, the claims providers put the client-credentials client in. Empty: any client
    // whose token holds the value.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::string client_id{};
};

struct ServiceClaimRefusal {
    std::string variable;
    std::string reason;
};

// Reads ULW_SERVICE_CLAIM, ULW_SERVICE_SCOPE and ULW_SERVICE_CLIENT_ID, each nullopt when unset
// or empty. The claim is 1 to 64 of A-Z a-z 0-9 _ . : / -, and none of the registered claims
// that every token carries with another meaning (iss, aud, exp, nbf, iat, jti); the value and
// the client id 1 to 128 printable ASCII characters without spaces, since a scope claim
// separates its values with them. No scope is no service, whatever else is set, so a
// configuration that carries the default claim (`scope`, as every shipped config.env does) and
// an empty scope starts with the service off. A claim other than the default without a scope is
// refused: a deployment that names one means to take service calls.
[[nodiscard]] std::expected<ServiceClaim, ServiceClaimRefusal>
read_service_claim(std::optional<std::string_view> claim, std::optional<std::string_view> scope,
                   std::optional<std::string_view> client_id = std::nullopt);

// Whether a token's claims name `client_id` as the client it was issued to, in `azp` (OpenID
// Connect, Keycloak) or `client_id` (RFC 9068, Okta). An empty `client_id` asks nothing.
[[nodiscard]] bool names_client(const core::json::Value& claims,
                                std::string_view client_id) noexcept;

// Whether a claim's value holds `value`: a string equal to it, or listing it among values
// separated by spaces (OAuth's `scope`, RFC 8693 section 4.2); an array with a string that
// does either; or `true` when `value` is "true". Any other shape holds nothing.
[[nodiscard]] bool claim_holds(const core::json::Value& claim, std::string_view value) noexcept;

} // namespace infra::auth
