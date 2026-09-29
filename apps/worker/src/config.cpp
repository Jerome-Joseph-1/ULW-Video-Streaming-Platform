#include "config.hpp"

#include "core/util/parse.hpp"
#include "infra/postgres/connection_string.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/profile.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace worker {

namespace {

constexpr std::array kSettings{
    ops::Setting{.env = "ULW_DATABASE_URL", .key = "database.url", .secret = true},
    ops::Setting{.env = "ULW_STORAGE", .key = "storage.backend"},
    ops::Setting{.env = "ULW_R2_ACCOUNT_ID", .key = "storage.r2_account_id"},
    ops::Setting{.env = "ULW_S3_ENDPOINT", .key = "storage.s3_endpoint"},
    ops::Setting{.env = "ULW_FS_ROOT", .key = "storage.fs_root"},
    ops::Setting{.env = "ULW_BUCKET", .key = "storage.bucket"},
    ops::Setting{.env = "ULW_NODE_ID", .key = "worker.node_id"},
    ops::Setting{.env = "HOSTNAME", .key = ""},
    ops::Setting{.env = "ULW_SCRATCH_DIR", .key = "worker.scratch_dir"},
    ops::Setting{.env = "ULW_SANDBOX_BIN", .key = "ffmpeg.sandbox_bin"},
    ops::Setting{.env = "ULW_FFMPEG", .key = "ffmpeg.ffmpeg"},
    ops::Setting{.env = "ULW_FFPROBE", .key = "ffmpeg.ffprobe"},
    ops::Setting{.env = "ULW_FFMPEG_THREADS", .key = "ffmpeg.threads"},
    ops::Setting{.env = "PATH", .key = ""},
    ops::Setting{.env = "ULW_LOG_LEVEL", .key = "log.level"},
    ops::Setting{.env = "ULW_RUN_AS_USER", .key = "process.user"},
    ops::Setting{.env = "ULW_ALLOW_ROOT", .key = "process.allow_root"},
    // Read by the store's credential provider; here only to be checked for.
    ops::Setting{.env = "ULW_S3_ACCESS_KEY_ID", .key = "", .secret = true},
    ops::Setting{.env = "ULW_S3_SECRET_ACCESS_KEY", .key = "", .secret = true},
};

// /var/tmp rather than /tmp: it survives reboots and is disk, where /tmp is often a tmpfs
// sized in megabytes, and a workspace holds a whole upload.
constexpr std::string_view kDefaultScratch = "/var/tmp/ulw-worker";
constexpr std::string_view kDefaultPath = "/usr/local/bin:/usr/bin:/bin";
// A number of our own: the host's core count says nothing of the container's CPU quota, and
// x264's memory grows with its threads. Four held the full ladder of a 1080p source at 556 MB
// resident, inside the 2 GB a worker is sized at (about 200 MB per rendition, brief 8.1), at
// 0.73x realtime on 4 cores.
constexpr unsigned kDefaultThreads = 4;
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
    const auto profile = storage.backend == StorageBackend::R2
                             ? infra::s3util::S3Profile::r2(storage.location)
                             : infra::s3util::S3Profile::minio(storage.location);
    if (!profile) {
        return error(location_variable, storage.backend == StorageBackend::R2
                                            ? "not an R2 account id"
                                            : "not an http or https endpoint URL");
    }
    for (const std::string_view key : {"ULW_S3_ACCESS_KEY_ID", "ULW_S3_SECRET_ACCESS_KEY"}) {
        if (!lookup(env, key)) {
            return error(key, "not set");
        }
    }
    // The same rules the start applies, so a key id it would refuse is refused here with exit
    // 2 rather than at the start with exit 1 and a restart loop. Neither value is quoted back.
    if (!infra::s3util::Credentials::make(
            *lookup(env, "ULW_S3_ACCESS_KEY_ID"),
            infra::s3util::SecretString(*lookup(env, "ULW_S3_SECRET_ACCESS_KEY")))) {
        return error("ULW_S3_ACCESS_KEY_ID",
                     "not an access key id: 1 to 128 letters, digits and -._~");
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

std::span<const ops::Setting> settings() noexcept {
    return kSettings;
}

std::expected<Config, ConfigError> load_config(const EnvLookup& env) {
    ops::Level level = ops::Level::Info;
    if (const auto text = lookup(env, "ULW_LOG_LEVEL")) {
        const auto parsed = ops::parse_level(*text);
        if (!parsed) {
            return error("ULW_LOG_LEVEL", "expected debug, info, warn or error");
        }
        level = *parsed;
    }
    auto database = required(env, "ULW_DATABASE_URL");
    if (!database) {
        return std::unexpected(std::move(database.error()));
    }
    // The reason libpq would give quotes the string, password and all.
    if (!infra::postgres::connection_string_parses(*database)) {
        return error("ULW_DATABASE_URL", "not a connection string libpq can read");
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
    const std::string allow_root = lookup(env, "ULW_ALLOW_ROOT").value_or("0");
    if (allow_root != "0" && allow_root != "1") {
        return error("ULW_ALLOW_ROOT", "expected 0 or 1");
    }
    unsigned threads = kDefaultThreads;
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
                  // Startup clears it: two workers given one scratch root, as by the
                  // default, must not clear each other's jobs.
                  .scratch = *scratch / node->view(),
                  .sandbox = std::move(*sandbox),
                  .ffmpeg = lookup(env, "ULW_FFMPEG").value_or("ffmpeg"),
                  .ffprobe = lookup(env, "ULW_FFPROBE").value_or("ffprobe"),
                  .search_path = lookup(env, "PATH").value_or(std::string(kDefaultPath)),
                  .ffmpeg_threads = threads,
                  .log_level = level,
                  .run_as_user = lookup(env, "ULW_RUN_AS_USER").value_or(""),
                  .allow_root = allow_root == "1"};
}

void log_effective(const Config& config, const ops::Settings& layers, ops::Logger& log) {
    const auto [storage,
                location_variable] = [&]() -> std::pair<std::string_view, std::string_view> {
        switch (config.storage) {
        case StorageBackend::R2:
            return {"r2", "ULW_R2_ACCOUNT_ID"};
        case StorageBackend::Minio:
            return {"minio", "ULW_S3_ENDPOINT"};
        case StorageBackend::Filesystem:
            return {"fs", "ULW_FS_ROOT"};
        }
        return {"r2", "ULW_R2_ACCOUNT_ID"};
    }();
    const std::array<std::pair<std::string_view, std::string>, 14> values{{
        {"ULW_DATABASE_URL", config.database_url},
        {"ULW_STORAGE", std::string(storage)},
        {location_variable, config.storage_location},
        {"ULW_BUCKET", config.bucket},
        {"ULW_NODE_ID", std::string(config.node.view())},
        {"ULW_SCRATCH_DIR", config.scratch.parent_path().string()},
        {"ULW_SANDBOX_BIN", config.sandbox.string()},
        {"ULW_FFMPEG", config.ffmpeg},
        {"ULW_FFPROBE", config.ffprobe},
        {"ULW_FFMPEG_THREADS", std::to_string(config.ffmpeg_threads)},
        {"PATH", config.search_path},
        {"ULW_LOG_LEVEL", std::string(ops::to_string(config.log_level))},
        {"ULW_RUN_AS_USER", config.run_as_user},
        {"ULW_ALLOW_ROOT", config.allow_root ? "1" : ""},
    }};
    for (const auto& [variable, value] : values) {
        if (value.empty()) {
            continue;
        }
        const bool secret = std::ranges::any_of(
            kSettings, [&](const ops::Setting& s) { return s.env == variable && s.secret; });
        // The node id may have come from HOSTNAME.
        const ops::Origin origin = variable == "ULW_NODE_ID" && !layers.get("ULW_NODE_ID")
                                       ? layers.origin("HOSTNAME")
                                       : layers.origin(variable);
        log.info("setting", {{"name", variable},
                             {"value", secret ? std::string_view("<redacted>") : value},
                             {"from", ops::to_string(origin)}});
    }
}

} // namespace worker
