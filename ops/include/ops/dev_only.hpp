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
// place of Askedin's JWKS, say), may be used. It may only with ULW_DEV_MODE=1, and never inside
// a Kubernetes pod, which is where every real deployment runs: the kubelet sets
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

} // namespace ops
