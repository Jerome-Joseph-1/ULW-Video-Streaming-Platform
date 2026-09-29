#pragma once

#include "core/models/ids.hpp"

#include "ops/log.hpp"
#include "ops/settings.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
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
    ops::Level log_level = ops::Level::Info;
    // Who to become when started as root.
    std::string run_as_user;
    // Stay root when started as root with no run_as_user; otherwise that is refused.
    bool allow_root = false;
};

struct ConfigError {
    std::string variable;
    std::string reason;
};

// The variable's value, or nullopt when unset. An empty value counts as unset.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Every value the worker reads, by the environment variable deployments set it with. The
// database URL carries a password, so it never comes from the command line.
[[nodiscard]] std::span<const ops::Setting> settings() noexcept;

// `env` looks values up by variable name, from the layered settings or the environment alone.
[[nodiscard]] std::expected<Config, ConfigError> load_config(const EnvLookup& env);

// One line per value, secrets redacted, saying where each came from.
void log_effective(const Config& config, const ops::Settings& layers, ops::Logger& log);

} // namespace worker
