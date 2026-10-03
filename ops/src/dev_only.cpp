#include "ops/dev_only.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace ops {

std::expected<void, DevOnlyRefusal> allow_dev_only(std::string_view variable,
                                                   const DevLookup& env) {
    const std::optional<std::string> mode = env("ULW_DEV_MODE");
    const bool dev = mode && *mode == "1";
    if (mode && !mode->empty() && *mode != "0" && !dev) {
        return std::unexpected(
            DevOnlyRefusal{.variable = "ULW_DEV_MODE", .reason = "expected 0 or 1"});
    }
    const std::optional<std::string> value = env(variable);
    if (!value || value->empty()) {
        return {};
    }
    if (const std::optional<std::string> cluster = env("KUBERNETES_SERVICE_HOST");
        cluster && !cluster->empty()) {
        return std::unexpected(
            DevOnlyRefusal{.variable = std::string(variable),
                           .reason = "development only, and this runs in a Kubernetes pod"});
    }
    if (!dev) {
        return std::unexpected(DevOnlyRefusal{
            .variable = std::string(variable),
            .reason = "development only; set ULW_DEV_MODE=1 where that is what this is"});
    }
    return {};
}

std::expected<KeySource, DevOnlyRefusal> key_source(const DevLookup& env) {
    const auto refuse = [](std::string variable, std::string reason) {
        return std::unexpected(
            DevOnlyRefusal{.variable = std::move(variable), .reason = std::move(reason)});
    };
    std::optional<std::string> url = env("JWKS_URL");
    std::optional<std::string> file = env("ULW_DEV_JWKS_FILE");
    // An empty value counts as unset, as deployment tools clear one that way.
    if (url && url->empty()) {
        url.reset();
    }
    if (file && file->empty()) {
        file.reset();
    }
    if (url && file) {
        return refuse("JWKS_URL", "set together with ULW_DEV_JWKS_FILE; choose one");
    }
    if (!url && !file) {
        return refuse("JWKS_URL", "not set");
    }
    // Over plain HTTP anyone on the path could hand us their own keys and sign any identity.
    if (url && !url->starts_with("https://")) {
        return refuse("JWKS_URL", "must be an https URL");
    }
    // A local key set signs any identity its holder likes; in a real deployment it would be a
    // way in, not a convenience.
    if (auto r = allow_dev_only("ULW_DEV_JWKS_FILE", env); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return KeySource{.url = std::move(url).value_or(""), .file = std::move(file).value_or("")};
}

std::expected<TokenRules, DevOnlyRefusal> token_rules(const DevLookup& env, const KeySource& keys) {
    constexpr std::size_t kMaxClaimName = 64;
    const auto value = [&env](std::string_view name) {
        std::optional<std::string> v = env(name);
        return v && !v->empty() ? std::move(v) : std::nullopt;
    };
    std::optional<std::string> audience = value("JWT_AUDIENCE");
    if (!audience && !keys.url.empty()) {
        return std::unexpected(DevOnlyRefusal{
            .variable = "JWT_AUDIENCE",
            .reason = "not set; required with JWKS_URL: the aud the identity provider puts in "
                      "tokens meant for this service"});
    }
    std::string claim = value("ULW_JWT_SUBJECT_CLAIM").value_or("sub");
    const auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == ':' || c == '/' || c == '-';
    };
    if (claim.size() > kMaxClaimName || !std::ranges::all_of(claim, allowed)) {
        return std::unexpected(
            DevOnlyRefusal{.variable = "ULW_JWT_SUBJECT_CLAIM",
                           .reason = "expected a claim name: 1 to 64 of A-Z a-z 0-9 _ . : / -"});
    }
    // Claims every token carries with another meaning: as the subject they would make all of a
    // provider's users one identity (iss, aud) or every token a new user (exp, nbf, iat, jti).
    constexpr std::array<std::string_view, 6> kReserved{"iss", "aud", "exp", "nbf", "iat", "jti"};
    if (std::ranges::find(kReserved, claim) != kReserved.end()) {
        return std::unexpected(DevOnlyRefusal{
            .variable = "ULW_JWT_SUBJECT_CLAIM",
            .reason = "a registered claim that does not name a user (iss, aud, exp, nbf, iat, "
                      "jti); expected sub or the claim that holds the user's id"});
    }
    return TokenRules{.audience = std::move(audience).value_or(std::string(kDevAudience)),
                      .subject_claim = std::move(claim)};
}

} // namespace ops
