#include "infra/auth/service_claim.hpp"

#include "core/util/json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace infra::auth {

namespace {

constexpr std::size_t kMaxClaimName = 64;
constexpr std::size_t kMaxValue = 128;

std::unexpected<ServiceClaimRefusal> refuse(std::string_view variable, std::string_view reason) {
    return std::unexpected(
        ServiceClaimRefusal{.variable = std::string(variable), .reason = std::string(reason)});
}

bool claim_name_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == ':' || c == '/' || c == '-';
}

// Printable ASCII but the space.
bool value_char(char c) noexcept {
    return c > ' ' && c < 0x7F;
}

// `text` is `value`, or lists it among values separated by spaces.
bool lists(std::string_view text, std::string_view value) noexcept {
    while (!text.empty()) {
        const std::size_t space = text.find(' ');
        if (text.substr(0, space) == value) {
            return true;
        }
        if (space == std::string_view::npos) {
            return false;
        }
        text.remove_prefix(space + 1);
    }
    return false;
}

} // namespace

std::expected<ServiceClaim, ServiceClaimRefusal>
read_service_claim(std::optional<std::string_view> claim, std::optional<std::string_view> scope,
                   std::optional<std::string_view> client_id) {
    ServiceClaim out;
    if (client_id) {
        if (client_id->size() > kMaxValue || !std::ranges::all_of(*client_id, value_char)) {
            return refuse("ULW_SERVICE_CLIENT_ID",
                          "expected 1 to 128 printable ASCII characters without spaces");
        }
        out.client_id = std::string(*client_id);
    }
    if (claim) {
        if (claim->size() > kMaxClaimName || !std::ranges::all_of(*claim, claim_name_char)) {
            return refuse("ULW_SERVICE_CLAIM",
                          "expected a claim name: 1 to 64 of A-Z a-z 0-9 _ . : / -");
        }
        // Every token carries these, and with another meaning.
        constexpr std::array<std::string_view, 6> kReserved{"iss", "aud", "exp",
                                                            "nbf", "iat", "jti"};
        if (std::ranges::find(kReserved, *claim) != kReserved.end()) {
            return refuse("ULW_SERVICE_CLAIM",
                          "a registered claim every token carries (iss, aud, exp, nbf, iat, "
                          "jti); expected the claim the identity provider puts scopes or roles in");
        }
        out.claim = std::string(*claim);
    }
    if (!scope) {
        if (claim && *claim != ServiceClaim{}.claim) {
            return refuse("ULW_SERVICE_CLAIM", "set, but ULW_SERVICE_SCOPE is not");
        }
        return out;
    }
    if (scope->size() > kMaxValue || !std::ranges::all_of(*scope, value_char)) {
        return refuse("ULW_SERVICE_SCOPE",
                      "expected 1 to 128 printable ASCII characters without spaces");
    }
    out.value = std::string(*scope);
    return out;
}

bool names_client(const core::json::Value& claims, std::string_view client_id) noexcept {
    if (client_id.empty()) {
        return true;
    }
    return std::ranges::any_of(std::array<std::string_view, 2>{"azp", "client_id"},
                               [&](std::string_view name) {
                                   const core::json::Value* v = claims.find(name);
                                   return v != nullptr && v->as_string() == client_id;
                               });
}

bool claim_holds(const core::json::Value& claim, std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (const auto text = claim.as_string()) {
        return lists(*text, value);
    }
    if (const auto flag = claim.as_bool()) {
        return *flag && value == "true";
    }
    const std::vector<core::json::Value>* items = claim.as_array();
    return items != nullptr && std::ranges::any_of(*items, [&](const core::json::Value& item) {
               const auto text = item.as_string();
               return text && lists(*text, value);
           });
}

} // namespace infra::auth
