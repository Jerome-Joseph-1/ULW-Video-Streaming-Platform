#pragma once

#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace reaper {

enum class StorageBackend : std::uint8_t { R2, Minio, Filesystem };

struct Config {
    std::string database_url;
    StorageBackend storage = StorageBackend::R2;
    // The R2 account id, the MinIO endpoint URL, or the filesystem root.
    std::string storage_location;
    std::string bucket;
    // How long the gateway lets an upload live, plus a margin; see Options::orphan_after.
    core::Seconds orphan_after{};
};

struct ConfigError {
    std::string variable;
    std::string reason;
};

using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// From the environment, as for the worker and gateway: the database URL carries a password.
[[nodiscard]] std::expected<Config, ConfigError> load_config(const EnvLookup& env);

} // namespace reaper
