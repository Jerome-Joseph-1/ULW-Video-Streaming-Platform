#include "config.hpp"

#include "core/util/parse.hpp"

#include <algorithm>
#include <utility>

namespace worker {

namespace {

// /var/tmp rather than /tmp: it survives reboots and is disk, where /tmp is often a tmpfs
// sized in megabytes, and a workspace holds a whole upload.
constexpr std::string_view kDefaultScratch = "/var/tmp/ulw-worker";
constexpr std::string_view kDefaultPath = "/usr/local/bin:/usr/bin:/bin";
// Past this, x264's frame threads stop scaling and only multiply the lookahead memory.
constexpr unsigned kMaxThreads = 64;

std::unexpected<ConfigError> error(std::string_view variable, std::string_view reason) {
    return std::unexpected(
        ConfigError{.variable = std::string(variable), .reason = std::string(reason)});
}

std::optional<std::string> lookup(const EnvLookup& env, std::string_view name) {
    auto value = env(name);
    if (value && value->empty()) {
        return std::nullopt;
    }
    return value;
}

std::expected<std::string, ConfigError> required(const EnvLookup& env, std::string_view name) {
    auto value = lookup(env, name);
    if (!value) {
        return error(name, "not set");
    }
    return std::move(*value);
}

std::expected<std::filesystem::path, ConfigError>
absolute_path(const EnvLookup& env, std::string_view name, std::string_view fallback) {
    std::filesystem::path p = lookup(env, name).value_or(std::string(fallback));
    if (!p.empty() && !p.is_absolute()) {
        return error(name, "must be an absolute path");
    }
    return p;
}

struct Storage {
    StorageBackend backend;
    std::string location;
    std::string bucket;
};

std::expected<Storage, ConfigError> load_storage(const EnvLookup& env) {
    const std::string kind = lookup(env, "ULW_STORAGE").value_or("r2");
    Storage storage{.backend = StorageBackend::R2, .location = {}, .bucket = {}};
    std::string_view location_variable;
    if (kind == "r2") {
        location_variable = "ULW_R2_ACCOUNT_ID";
    } else if (kind == "minio") {
        storage.backend = StorageBackend::Minio;
        location_variable = "ULW_S3_ENDPOINT";
    } else if (kind == "fs") {
        storage.backend = StorageBackend::Filesystem;
        location_variable = "ULW_FS_ROOT";
    } else {
        return error("ULW_STORAGE", "expected r2, minio or fs");
    }
    auto location = required(env, location_variable);
    if (!location) {
        return std::unexpected(std::move(location.error()));
    }
    storage.location = std::move(*location);
    if (storage.backend == StorageBackend::Filesystem) {
        return storage;
    }
    auto bucket = required(env, "ULW_BUCKET");
    if (!bucket) {
        return std::unexpected(std::move(bucket.error()));
    }
    storage.bucket = std::move(*bucket);
    return storage;
}

// Kubernetes sets HOSTNAME to the pod name, which is what the lease should name.
std::expected<core::NodeId, ConfigError> load_node(const EnvLookup& env) {
    const auto explicit_name = lookup(env, "ULW_NODE_ID");
    const std::string_view variable = explicit_name ? "ULW_NODE_ID" : "HOSTNAME";
    const auto name = explicit_name ? explicit_name : lookup(env, "HOSTNAME");
    if (!name) {
        return error("ULW_NODE_ID", "not set, and neither is HOSTNAME");
    }
    auto node = core::NodeId::parse(*name);
    if (!node) {
        return error(variable, "not an RFC 1123 label");
    }
    return *node;
}

} // namespace

std::expected<Config, ConfigError> load_config(const EnvLookup& env, unsigned cores) {
    auto database = required(env, "ULW_DATABASE_URL");
    if (!database) {
        return std::unexpected(std::move(database.error()));
    }
    auto storage = load_storage(env);
    if (!storage) {
        return std::unexpected(std::move(storage.error()));
    }
    const auto node = load_node(env);
    if (!node) {
        return std::unexpected(node.error());
    }
    auto scratch = absolute_path(env, "ULW_SCRATCH_DIR", kDefaultScratch);
    if (!scratch) {
        return std::unexpected(std::move(scratch.error()));
    }
    auto sandbox = absolute_path(env, "ULW_SANDBOX_BIN", "");
    if (!sandbox) {
        return std::unexpected(std::move(sandbox.error()));
    }
    unsigned threads = std::clamp(cores, 1U, kMaxThreads);
    if (const auto text = lookup(env, "ULW_FFMPEG_THREADS")) {
        const auto value = core::parse_integer<unsigned>(*text);
        if (!value || *value < 1 || *value > kMaxThreads) {
            return error("ULW_FFMPEG_THREADS", "not an integer in 1..64");
        }
        threads = *value;
    }
    return Config{.database_url = std::move(*database),
                  .storage = storage->backend,
                  .storage_location = std::move(storage->location),
                  .bucket = std::move(storage->bucket),
                  .node = *node,
                  .scratch = std::move(*scratch),
                  .sandbox = std::move(*sandbox),
                  .ffmpeg = lookup(env, "ULW_FFMPEG").value_or("ffmpeg"),
                  .ffprobe = lookup(env, "ULW_FFPROBE").value_or("ffprobe"),
                  .search_path = lookup(env, "PATH").value_or(std::string(kDefaultPath)),
                  .ffmpeg_threads = threads};
}

} // namespace worker
