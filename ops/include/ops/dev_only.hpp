#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ops {

struct DevOnlyRefusal {
    // The variable at fault.
    std::string variable;
    std::string reason;
};

// A value by variable name, or nullopt when unset; an empty value counts as unset.
using DevLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Whether `variable`, a setting that exists for development alone (a local key set trusted in
// place of the identity provider's JWKS, say), may be used. It may only with ULW_DEV_MODE=1, and
// never inside a Kubernetes pod, which is where every real deployment runs: the kubelet sets
// KUBERNETES_SERVICE_HOST in every container, and no manifest of ours can unset it. ULW_DEV_MODE
// is "0" or "1", unset meaning "0"; anything else is refused whether or not `variable` is set.
[[nodiscard]] std::expected<void, DevOnlyRefusal> allow_dev_only(std::string_view variable,
                                                                 const DevLookup& env);

// Where a server's token keys come from: exactly one of the two is set, the other empty.
struct KeySource {
    // JWKS_URL, always https://.
    std::string url;
    // ULW_DEV_JWKS_FILE, allowed by allow_dev_only.
    std::string file;
};

// Reads JWKS_URL and ULW_DEV_JWKS_FILE, as the gateway and the chat server both take them.
// Refused: both set, neither set, a URL that is not https, or a local key set where
// allow_dev_only says no; the refusal names the variable at fault.
[[nodiscard]] std::expected<KeySource, DevOnlyRefusal> key_source(const DevLookup& env);

// The `aud` a local key set's tokens are checked for when JWT_AUDIENCE is unset, and what
// ulw_devtoken mints by default. Never a default against a real identity provider's JWKS.
inline constexpr std::string_view kDevAudience = "ulw-dev";

// What a verified token must name, beyond its issuer.
struct TokenRules {
    // JWT_AUDIENCE.
    std::string audience;
    // ULW_JWT_SUBJECT_CLAIM: the claim that carries the user's identifier.
    std::string subject_claim;
};

// Reads JWT_AUDIENCE and ULW_JWT_SUBJECT_CLAIM, as the gateway and the chat server both take
// them, for keys from `keys`. With JWKS_URL, JWT_AUDIENCE is required: every identity provider
// names its own, and a guessed default would either refuse every token or accept another
// service's. With a local key set it defaults to kDevAudience. ULW_JWT_SUBJECT_CLAIM defaults to
// `sub` and is 1 to 64 of A-Z a-z 0-9 and _ . : / - (the claim names identity providers use).
// The refusal names the variable at fault.
[[nodiscard]] std::expected<TokenRules, DevOnlyRefusal> token_rules(const DevLookup& env,
                                                                    const KeySource& keys);

} // namespace ops
