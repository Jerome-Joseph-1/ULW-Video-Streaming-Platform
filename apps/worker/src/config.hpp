#pragma once

#include "core/models/ids.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace worker {

enum class StorageBackend : std::uint8_t { R2, Minio, Filesystem };

struct Config {
    std::string database_url;
    StorageBackend storage = StorageBackend::R2;
    // The R2 account id, the MinIO endpoint URL, or the filesystem root.
    std::string storage_location;
    std::string bucket;
    // The lease holder's name in the jobs table.
    core::NodeId node;
    // Workspaces go under it: ULW_SCRATCH_DIR/<node>, this worker's alone, which startup
    // clears.
    std::filesystem::path scratch;
    // The ulw_sandbox helper; empty means the one installed beside this executable.
    std::filesystem::path sandbox;
    std::string ffmpeg;
    std::string ffprobe;
    // PATH for the sandboxed children, which inherit nothing else.
    std::string search_path;
    unsigned ffmpeg_threads = 1;
};

struct ConfigError {
    std::string variable;
    std::string reason;
};

// The variable's value, or nullopt when unset. An empty value counts as unset.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Everything comes from the environment, as for the gateway: arguments are visible to every
// user through /proc, and the database URL carries a password.
[[nodiscard]] std::expected<Config, ConfigError> load_config(const EnvLookup& env);

} // namespace worker
