#include "config.hpp"

#include "core/util/parse.hpp"

#include "ops/root.hpp"

#include <chrono>
#include <utility>

namespace reaper {

namespace {

// Gateway::Limits::upload_ttl, six days. An upload's session is at most that old while its row
// is active, and a day's margin covers a reaper that missed a pass.
constexpr std::uint64_t kDefaultUploadTtlHours = std::uint64_t{6} * 24;
constexpr std::chrono::hours kMargin{24};
// A year: far past any sensible ttl, and small enough that the hours cannot overflow a duration.
constexpr std::uint64_t kMaxUploadTtlHours = std::uint64_t{24} * 366;

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

} // namespace

std::expected<Config, ConfigError> load_config(const EnvLookup& env) {
    auto database = required(env, "ULW_DATABASE_URL");
    if (!database) {
        return std::unexpected(std::move(database.error()));
    }
    Config config{.database_url = std::move(*database),
                  .storage = StorageBackend::R2,
                  .storage_location = {},
                  .bucket = {},
                  .orphan_after = {},
                  .run_as_user = {},
                  .allow_root = false};
    const std::string kind = lookup(env, "ULW_STORAGE").value_or("r2");
    std::string_view location_variable;
    if (kind == "r2") {
        location_variable = "ULW_R2_ACCOUNT_ID";
    } else if (kind == "minio") {
        config.storage = StorageBackend::Minio;
        location_variable = "ULW_S3_ENDPOINT";
    } else if (kind == "fs") {
        config.storage = StorageBackend::Filesystem;
        location_variable = "ULW_FS_ROOT";
    } else {
        return error("ULW_STORAGE", "expected r2, minio or fs");
    }
    auto location = required(env, location_variable);
    if (!location) {
        return std::unexpected(std::move(location.error()));
    }
    config.storage_location = std::move(*location);
    if (config.storage != StorageBackend::Filesystem) {
        auto bucket = required(env, "ULW_BUCKET");
        if (!bucket) {
            return std::unexpected(std::move(bucket.error()));
        }
        config.bucket = std::move(*bucket);
    }
    std::uint64_t ttl_hours = kDefaultUploadTtlHours;
    if (const auto text = lookup(env, "ULW_UPLOAD_TTL_HOURS")) {
        const auto value = core::parse_integer<std::uint64_t>(*text);
        if (!value || *value == 0 || *value > kMaxUploadTtlHours) {
            return error("ULW_UPLOAD_TTL_HOURS", "not an integer in 1..8784");
        }
        ttl_hours = *value;
    }
    config.orphan_after = std::chrono::duration_cast<core::Seconds>(
        std::chrono::hours{static_cast<std::int64_t>(ttl_hours)} + kMargin);
    const auto allow_root = ops::parse_allow_root(lookup(env, "ULW_ALLOW_ROOT"));
    if (!allow_root) {
        return error("ULW_ALLOW_ROOT", "expected 0 or 1");
    }
    config.allow_root = *allow_root;
    config.run_as_user = lookup(env, "ULW_RUN_AS_USER").value_or("");
    return config;
}

} // namespace reaper
