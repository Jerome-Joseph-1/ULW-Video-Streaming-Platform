#include "config.hpp"

#include "core/util/parse.hpp"

#include <utility>

namespace gateway {

namespace {

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

template <class T>
std::expected<T, ConfigError> number(const EnvLookup& env, std::string_view name, T fallback, T min,
                                     T max) {
    const auto text = lookup(env, name);
    if (!text) {
        return fallback;
    }
    const auto value = core::parse_integer<T>(*text);
    if (!value || *value < min || *value > max) {
        return error(name, "not an integer in range");
    }
    return *value;
}

std::expected<void, ConfigError> load_storage(const EnvLookup& env, Config& config) {
    const std::string kind = lookup(env, "ULW_STORAGE").value_or("r2");
    std::string_view location_variable;
    if (kind == "r2") {
        config.storage = StorageBackend::R2;
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
    if (config.storage == StorageBackend::Filesystem) {
        return {};
    }
    auto bucket = required(env, "ULW_BUCKET");
    if (!bucket) {
        return std::unexpected(std::move(bucket.error()));
    }
    config.bucket = std::move(*bucket);
    return {};
}

std::expected<void, ConfigError> load_transport(const EnvLookup& env, Config& config) {
    const std::string kind = lookup(env, "ULW_TRANSPORT").value_or("plain");
    auto cert = lookup(env, "ULW_TLS_CERT_FILE");
    auto key = lookup(env, "ULW_TLS_KEY_FILE");
    if (kind == "plain") {
        // Files set for a transport that ignores them would mean a deployment that believes it
        // serves TLS and serves plaintext.
        if (cert || key) {
            return error(cert ? "ULW_TLS_CERT_FILE" : "ULW_TLS_KEY_FILE",
                         "set, but ULW_TRANSPORT is plain");
        }
        config.transport = Transport::Plain;
        return {};
    }
    if (kind != "tls") {
        return error("ULW_TRANSPORT", "expected plain or tls");
    }
    if (!cert) {
        return error("ULW_TLS_CERT_FILE", "not set; ULW_TRANSPORT=tls needs it");
    }
    if (!key) {
        return error("ULW_TLS_KEY_FILE", "not set; ULW_TRANSPORT=tls needs it");
    }
    config.transport = Transport::Tls;
    config.tls_certificate_chain = std::move(*cert);
    config.tls_private_key = std::move(*key);
    return {};
}

std::expected<void, ConfigError> load_auth(const EnvLookup& env, Config& config) {
    auto url = lookup(env, "JWKS_URL");
    auto file = lookup(env, "ULW_DEV_JWKS_FILE");
    if (url && file) {
        return error("JWKS_URL", "set together with ULW_DEV_JWKS_FILE; choose one");
    }
    if (!url && !file) {
        return error("JWKS_URL", "not set");
    }
    // Over plain HTTP anyone on the path could hand us their own keys and sign any identity.
    if (url && !url->starts_with("https://")) {
        return error("JWKS_URL", "must be an https URL");
    }
    config.jwks_url = std::move(url).value_or("");
    config.dev_jwks_file = std::move(file).value_or("");
    auto issuer = required(env, "JWT_ISSUER");
    if (!issuer) {
        return std::unexpected(std::move(issuer.error()));
    }
    config.jwt_issuer = std::move(*issuer);
    config.jwt_audience = lookup(env, "JWT_AUDIENCE").value_or("askedin-platform");
    config.limits.auth_cookie = lookup(env, "ULW_AUTH_COOKIE").value_or("auth_token");
    return {};
}

} // namespace

std::expected<Config, ConfigError> load_config(const EnvLookup& env) {
    Config config;
    // Port 0 would bind an ephemeral port nobody can be told about.
    const auto port = number<std::uint16_t>(env, "ULW_LISTEN_PORT", 8080, 1, 65'535);
    if (!port) {
        return std::unexpected(port.error());
    }
    config.port = *port;
    if (const auto name = lookup(env, "ULW_REACTOR")) {
        const auto kind = net::parse_reactor_kind(*name);
        if (!kind) {
            return error("ULW_REACTOR", "expected io_uring or epoll");
        }
        config.reactor = *kind;
    }
    if (auto r = load_transport(env, config); !r) {
        return std::unexpected(std::move(r.error()));
    }
    // More threads than this would only queue on the object store's per-host connection cap.
    const auto threads = number<std::size_t>(env, "ULW_OFFLOAD_THREADS", 4, 1, 64);
    if (!threads) {
        return std::unexpected(threads.error());
    }
    config.offload_threads = *threads;
    if (auto r = load_storage(env, config); !r) {
        return std::unexpected(std::move(r.error()));
    }
    auto database = required(env, "ULW_DATABASE_URL");
    if (!database) {
        return std::unexpected(std::move(database.error()));
    }
    config.database_url = std::move(*database);
    if (auto r = load_auth(env, config); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return config;
}

} // namespace gateway
