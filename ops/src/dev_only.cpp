#include "ops/dev_only.hpp"

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

} // namespace ops
