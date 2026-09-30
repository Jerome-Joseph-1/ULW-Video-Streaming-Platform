#include "ops/dev_only.hpp"

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

} // namespace ops
