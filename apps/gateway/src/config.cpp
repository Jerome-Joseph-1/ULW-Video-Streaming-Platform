#include "config.hpp"

#include "core/models/upload.hpp"
#include "core/util/parse.hpp"
#include "http/request_parser.hpp"
#include "infra/auth/local_verifier.hpp"
#include "infra/postgres/connection_string.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/profile.hpp"
#include "net/ip_address.hpp"
#include "net/transport.hpp"

#include "ops/dev_only.hpp"
#include "ops/root.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace gateway {

namespace {

constexpr std::array kSettings{
    ops::Setting{.env = "ULW_LISTEN_PORT", .key = "listen.port"},
    ops::Setting{.env = "ULW_REACTOR", .key = "listen.reactor"},
    ops::Setting{.env = "ULW_TRANSPORT", .key = "listen.transport"},
    ops::Setting{.env = "ULW_TLS_CERT_FILE", .key = "tls.cert_file"},
    ops::Setting{.env = "ULW_TLS_KEY_FILE", .key = "tls.key_file"},
    ops::Setting{.env = "ULW_OFFLOAD_THREADS", .key = "limits.offload_threads"},
    ops::Setting{.env = "ULW_MAX_CONNECTIONS", .key = "limits.max_connections"},
    ops::Setting{.env = "ULW_MAX_UPLOAD_SLOTS", .key = "limits.max_upload_slots"},
    ops::Setting{.env = "ULW_MAX_UPLOADS_PER_USER", .key = "limits.max_uploads_per_user"},
    ops::Setting{.env = "ULW_MAX_CONNECTIONS_PER_IP", .key = "limits.max_connections_per_ip"},
    ops::Setting{.env = "ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND",
                 .key = "limits.new_connections_per_ip_per_second"},
    ops::Setting{.env = "ULW_REQUESTS_PER_USER_PER_MINUTE",
                 .key = "limits.requests_per_user_per_minute"},
    ops::Setting{.env = "ULW_UPLOAD_BYTES_PER_USER_PER_DAY",
                 .key = "limits.upload_bytes_per_user_per_day"},
    ops::Setting{.env = "ULW_TRUSTED_PROXIES", .key = "limits.trusted_proxies"},
    ops::Setting{.env = "ULW_TRUSTED_PROXY_HOPS", .key = "limits.trusted_proxy_hops"},
    ops::Setting{.env = "ULW_RUN_AS_USER", .key = "process.user"},
    ops::Setting{.env = "ULW_ALLOW_ROOT", .key = "process.allow_root"},
    ops::Setting{.env = "ULW_STORAGE", .key = "storage.backend"},
    ops::Setting{.env = "ULW_R2_ACCOUNT_ID", .key = "storage.r2_account_id"},
    ops::Setting{.env = "ULW_S3_ENDPOINT", .key = "storage.s3_endpoint"},
    ops::Setting{.env = "ULW_FS_ROOT", .key = "storage.fs_root"},
    ops::Setting{.env = "ULW_FS_READ_URL", .key = "storage.fs_read_url"},
    ops::Setting{.env = "ULW_BUCKET", .key = "storage.bucket"},
    ops::Setting{.env = "ULW_CHUNK_SIZE", .key = "storage.chunk_size"},
    ops::Setting{.env = "ULW_DATABASE_URL", .key = "database.url", .secret = true},
    ops::Setting{.env = "JWKS_URL", .key = "auth.jwks_url"},
    ops::Setting{.env = "ULW_DEV_JWKS_FILE", .key = "auth.dev_jwks_file"},
    ops::Setting{.env = "ULW_DEV_MODE", .key = "dev.mode"},
    // Not a setting: the kubelet sets it in every container, and it is read only to refuse
    // development settings there.
    ops::Setting{.env = "KUBERNETES_SERVICE_HOST", .key = ""},
    ops::Setting{.env = "JWT_ISSUER", .key = "auth.issuer"},
    ops::Setting{.env = "JWT_AUDIENCE", .key = "auth.audience"},
    ops::Setting{.env = "ULW_AUTH_COOKIE", .key = "auth.cookie"},
    ops::Setting{.env = "ULW_LOG_LEVEL", .key = "log.level"},
    // Read by the store's credential provider; here only to be checked for.
    ops::Setting{.env = "ULW_S3_ACCESS_KEY_ID", .key = "", .secret = true},
    ops::Setting{.env = "ULW_S3_SECRET_ACCESS_KEY", .key = "", .secret = true},
};

// S3 and R2 refuse a part under 5 MiB unless it is the last, and above 5 GiB.
constexpr std::uint64_t kMinChunk = std::uint64_t{5} << 20U;
constexpr std::uint64_t kMaxChunk = std::uint64_t{5} << 30U;
// Neither allows more than 10,000 parts, so the largest upload bounds the chunk from below:
// 50 GiB / 10,000 is 5.12 MiB, just above the store's own minimum.
constexpr std::uint64_t kMaxParts = 10'000;
constexpr std::uint64_t kDescriptorReserve = 64;
// Every trusted block is tried against every accepted peer; a deployment names one or two.
constexpr std::size_t kMaxTrustedProxies = 16;
// Past a CDN, a load balancer and Envoy there is no chain worth trusting.
constexpr std::size_t kMaxProxyHops = 16;
// Blocks wider than these are rarely one's own proxies: a /8 of IPv4 is 16 million addresses,
// and a /32 of IPv6 a whole provider's allocation.
constexpr unsigned kWideV4Prefix = 8;
constexpr unsigned kWideV6Prefix = 32;
// A bucket's count is a double, exact to 2^53: 8 PiB a day is past any quota worth setting.
constexpr std::uint64_t kMaxDailyBytes = std::uint64_t{1} << 53U;

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
    auto read_url = lookup(env, "ULW_FS_READ_URL");
    if (config.storage == StorageBackend::Filesystem) {
        if (read_url && !read_url->starts_with("http://") && !read_url->starts_with("https://")) {
            return error("ULW_FS_READ_URL", "must be an http or https URL");
        }
        // Trailing slashes are dropped so the key can follow a single one.
        while (read_url && read_url->ends_with('/')) {
            read_url->pop_back();
        }
        config.limits.local_read_url = std::move(read_url).value_or("");
        return {};
    }
    // An object store signs its own URLs; a file server set beside one would never be used.
    if (read_url) {
        return error("ULW_FS_READ_URL", "set, but ULW_STORAGE is not fs");
    }
    const auto profile = config.storage == StorageBackend::R2
                             ? infra::s3util::S3Profile::r2(config.storage_location)
                             : infra::s3util::S3Profile::minio(config.storage_location);
    if (!profile) {
        return error(location_variable, config.storage == StorageBackend::R2
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
    // Read now, as the listener would: a certificate that does not load, or does not match
    // its key, would otherwise fail the process after the checks said it could start.
    if (auto r = net::check_tls_files({.certificate_chain = *cert, .private_key = *key}); !r) {
        return error("ULW_TLS_CERT_FILE", r.error());
    }
    config.transport = Transport::Tls;
    config.tls_certificate_chain = std::move(*cert);
    config.tls_private_key = std::move(*key);
    return {};
}

std::optional<std::string> read_key_set(const std::string& path) {
    // A development key set holds one or two Ed25519 keys, a few hundred bytes.
    constexpr std::size_t kMaxKeySet = std::size_t{64} * 1024;
    std::ifstream in(path, std::ios::binary);
    std::string out;
    // One page per read; the whole file is at most sixteen of them.
    std::array<char, 4096> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.append(buf.data(), static_cast<std::size_t>(in.gcount()));
        if (out.size() > kMaxKeySet) {
            return std::nullopt;
        }
    }
    if (!in.eof()) {
        return std::nullopt;
    }
    return out;
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
    // A local key set signs any identity its holder likes; in a real deployment it would be a
    // way in, not a convenience.
    if (auto r = ops::allow_dev_only("ULW_DEV_JWKS_FILE", env); !r) {
        return error(r.error().variable, r.error().reason);
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
    if (config.dev_jwks_file.empty()) {
        return {};
    }
    auto jwks = read_key_set(config.dev_jwks_file);
    if (!jwks) {
        return error("ULW_DEV_JWKS_FILE", "unreadable, or larger than 64 KiB");
    }
    const auto keys = infra::auth::Ed25519LocalVerifier::create(
        *jwks, {.issuer = config.jwt_issuer, .audience = config.jwt_audience});
    if (!keys) {
        return error("ULW_DEV_JWKS_FILE", infra::auth::to_string(keys.error()));
    }
    config.dev_jwks = std::move(*jwks);
    return {};
}

std::expected<std::vector<net::IpNetwork>, ConfigError> parse_proxies(std::string_view text) {
    std::vector<net::IpNetwork> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
            item.remove_prefix(1);
        }
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
            item.remove_suffix(1);
        }
        const auto network = net::IpNetwork::parse(item);
        if (!network) {
            return error("ULW_TRUSTED_PROXIES",
                         "expected comma-separated CIDR blocks, such as 10.42.0.0/16, with no "
                         "bits set past the prefix");
        }
        // Every address on the internet could then name any client it liked.
        if (network->prefix_length() == 0) {
            return error("ULW_TRUSTED_PROXIES", "a /0 block trusts every peer");
        }
        out.push_back(*network);
    }
    if (out.size() > kMaxTrustedProxies) {
        return error("ULW_TRUSTED_PROXIES", "more than 16 blocks");
    }
    return out;
}

std::expected<void, ConfigError> load_client_limits(const EnvLookup& env, Limits& limits) {
    // Past the connection limit a per-address one would never be reached.
    const auto per_ip = number<std::size_t>(
        env, "ULW_MAX_CONNECTIONS_PER_IP",
        std::min(limits.max_connections_per_ip, limits.max_connections), 1, limits.max_connections);
    if (!per_ip) {
        return std::unexpected(per_ip.error());
    }
    limits.max_connections_per_ip = *per_ip;
    const auto rate = number<std::uint32_t>(env, "ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND",
                                            limits.new_connections_per_ip_per_second, 1, 65'536);
    if (!rate) {
        return std::unexpected(rate.error());
    }
    limits.new_connections_per_ip_per_second = *rate;
    const auto requests = number<std::uint32_t>(env, "ULW_REQUESTS_PER_USER_PER_MINUTE",
                                                limits.requests_per_user_per_minute, 1, 1'000'000);
    if (!requests) {
        return std::unexpected(requests.error());
    }
    limits.requests_per_user_per_minute = *requests;
    // Below the largest body a PATCH may carry, that PATCH could never be admitted at all.
    const auto bytes = number<std::uint64_t>(
        env, "ULW_UPLOAD_BYTES_PER_USER_PER_DAY", limits.upload_bytes_per_user_per_day,
        http::RequestParser::kMaxContentLength, kMaxDailyBytes);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    limits.upload_bytes_per_user_per_day = *bytes;
    if (const auto text = lookup(env, "ULW_TRUSTED_PROXIES")) {
        auto proxies = parse_proxies(*text);
        if (!proxies) {
            return std::unexpected(std::move(proxies.error()));
        }
        limits.trusted_proxies = std::move(*proxies);
    }
    // Envoy alone in front appends one entry; each further proxy, one more.
    const auto hops = number<std::size_t>(env, "ULW_TRUSTED_PROXY_HOPS", 1, 1, kMaxProxyHops);
    if (!hops) {
        return std::unexpected(hops.error());
    }
    if (lookup(env, "ULW_TRUSTED_PROXY_HOPS") && limits.trusted_proxies.empty()) {
        return error("ULW_TRUSTED_PROXY_HOPS", "set, but ULW_TRUSTED_PROXIES is not");
    }
    limits.trusted_proxy_hops = *hops;
    return {};
}

// Everything a limit is checked against is known here, so each is checked here, once.
std::expected<void, ConfigError> load_limits(const EnvLookup& env, Config& config) {
    Limits& limits = config.limits;
    // Past 65,536 the reactor's descriptor-indexed tables and the budget of ADR-0027 both stop
    // meaning anything.
    const auto connections =
        number<std::size_t>(env, "ULW_MAX_CONNECTIONS", limits.max_connections, 1, 65'536);
    if (!connections) {
        return std::unexpected(connections.error());
    }
    limits.max_connections = *connections;
    const auto slots = number<std::size_t>(
        env, "ULW_MAX_UPLOAD_SLOTS", std::min(limits.max_upload_slots, *connections), 1, 65'536);
    if (!slots) {
        return std::unexpected(slots.error());
    }
    // A slot is held by a connection; more slots than connections could never be used, and
    // would mean the operator believes the gateway takes more uploads than it can.
    if (*slots > limits.max_connections) {
        return error("ULW_MAX_UPLOAD_SLOTS", "above ULW_MAX_CONNECTIONS");
    }
    limits.max_upload_slots = *slots;
    const auto per_user = number<std::size_t>(
        env, "ULW_MAX_UPLOADS_PER_USER", std::min(limits.max_uploads_per_user, *slots), 1, 65'536);
    if (!per_user) {
        return std::unexpected(per_user.error());
    }
    if (*per_user > limits.max_upload_slots) {
        return error("ULW_MAX_UPLOADS_PER_USER", "above ULW_MAX_UPLOAD_SLOTS");
    }
    limits.max_uploads_per_user = *per_user;
    if (auto r = load_client_limits(env, limits); !r) {
        return r;
    }
    const auto chunk =
        number<std::uint64_t>(env, "ULW_CHUNK_SIZE", config.chunk_size, 1, kMaxChunk);
    if (!chunk) {
        return std::unexpected(chunk.error());
    }
    if (*chunk < kMinChunk) {
        return error("ULW_CHUNK_SIZE", "below the object store's 5 MiB minimum part");
    }
    if ((core::Upload::kMaxSizeBytes + *chunk - 1) / *chunk > kMaxParts) {
        return error("ULW_CHUNK_SIZE", "a 50 GiB upload would need more than 10,000 parts");
    }
    config.chunk_size = *chunk;
    return {};
}

} // namespace

std::span<const ops::Setting> settings() noexcept {
    return kSettings;
}

std::expected<Config, ConfigError> load_config(const EnvLookup& env) {
    Config config;
    if (const auto level = lookup(env, "ULW_LOG_LEVEL")) {
        const auto parsed = ops::parse_level(*level);
        if (!parsed) {
            return error("ULW_LOG_LEVEL", "expected debug, info, warn or error");
        }
        config.log_level = *parsed;
    }
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
    // The reason libpq would give quotes the string, password and all.
    if (!infra::postgres::connection_string_parses(*database)) {
        return error("ULW_DATABASE_URL", "not a connection string libpq can read");
    }
    config.database_url = std::move(*database);
    if (auto r = load_auth(env, config); !r) {
        return std::unexpected(std::move(r.error()));
    }
    if (auto r = load_limits(env, config); !r) {
        return std::unexpected(std::move(r.error()));
    }
    config.run_as_user = lookup(env, "ULW_RUN_AS_USER").value_or("");
    const auto allow_root = ops::parse_allow_root(lookup(env, "ULW_ALLOW_ROOT"));
    if (!allow_root) {
        return error("ULW_ALLOW_ROOT", "expected 0 or 1");
    }
    config.allow_root = *allow_root;
    return config;
}

std::expected<void, ConfigError> check_descriptor_budget(const Limits& limits, std::size_t nofile) {
    const std::uint64_t budget =
        nofile > kDescriptorReserve ? (nofile - kDescriptorReserve) / 2 : 0;
    if (limits.max_connections > budget) {
        return error("ULW_MAX_CONNECTIONS", "above the descriptor budget (RLIMIT_NOFILE " +
                                                std::to_string(nofile) +
                                                " - 64) / 2 = " + std::to_string(budget));
    }
    return {};
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
    // Checked as given; the blocks themselves hold no text to print back.
    const std::string proxies =
        config.limits.trusted_proxies.empty() ? "" : layers.get("ULW_TRUSTED_PROXIES").value_or("");
    const std::array<std::pair<std::string_view, std::string>, 30> values{{
        {"ULW_LISTEN_PORT", std::to_string(config.port)},
        {"ULW_REACTOR", std::string(net::to_string(config.reactor))},
        {"ULW_TRANSPORT", config.transport == Transport::Tls ? "tls" : "plain"},
        {"ULW_TLS_CERT_FILE", config.tls_certificate_chain},
        {"ULW_TLS_KEY_FILE", config.tls_private_key},
        {"ULW_OFFLOAD_THREADS", std::to_string(config.offload_threads)},
        {"ULW_MAX_CONNECTIONS", std::to_string(config.limits.max_connections)},
        {"ULW_MAX_UPLOAD_SLOTS", std::to_string(config.limits.max_upload_slots)},
        {"ULW_MAX_UPLOADS_PER_USER", std::to_string(config.limits.max_uploads_per_user)},
        {"ULW_MAX_CONNECTIONS_PER_IP", std::to_string(config.limits.max_connections_per_ip)},
        {"ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND",
         std::to_string(config.limits.new_connections_per_ip_per_second)},
        {"ULW_REQUESTS_PER_USER_PER_MINUTE",
         std::to_string(config.limits.requests_per_user_per_minute)},
        {"ULW_UPLOAD_BYTES_PER_USER_PER_DAY",
         std::to_string(config.limits.upload_bytes_per_user_per_day)},
        {"ULW_TRUSTED_PROXIES", proxies},
        {"ULW_TRUSTED_PROXY_HOPS",
         proxies.empty() ? "" : std::to_string(config.limits.trusted_proxy_hops)},
        {"ULW_RUN_AS_USER", config.run_as_user},
        {"ULW_ALLOW_ROOT", config.allow_root ? "1" : ""},
        {"ULW_STORAGE", std::string(storage)},
        {location_variable, config.storage_location},
        {"ULW_FS_READ_URL", config.limits.local_read_url},
        {"ULW_BUCKET", config.bucket},
        {"ULW_CHUNK_SIZE", std::to_string(config.chunk_size)},
        {"ULW_DATABASE_URL", config.database_url},
        {"JWKS_URL", config.jwks_url},
        {"ULW_DEV_JWKS_FILE", config.dev_jwks_file},
        {"JWT_ISSUER", config.jwt_issuer},
        {"JWT_AUDIENCE", config.jwt_audience},
        {"ULW_AUTH_COOKIE", config.limits.auth_cookie},
        {"ULW_LOG_LEVEL", std::string(ops::to_string(config.log_level))},
    }};
    for (const auto& [variable, value] : values) {
        if (value.empty()) {
            continue;
        }
        const bool secret = std::ranges::any_of(
            kSettings, [&](const ops::Setting& s) { return s.env == variable && s.secret; });
        log.info("setting", {{"name", variable},
                             {"value", secret ? std::string_view("<redacted>") : value},
                             {"from", ops::to_string(layers.origin(variable))}});
    }
    for (const net::IpNetwork& block : config.limits.trusted_proxies) {
        if (block.prefix_length() < (block.is_v4() ? kWideV4Prefix : kWideV6Prefix)) {
            log.warn("a trusted proxy block this wide lets many peers name any client",
                     {{"prefix_length", block.prefix_length()}});
        }
    }
}

} // namespace gateway
