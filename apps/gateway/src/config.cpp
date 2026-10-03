#include "config.hpp"

#include "core/models/upload.hpp"
#include "core/util/parse.hpp"
#include "http/client_limits.hpp"
#include "http/origin.hpp"
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
    ops::Setting{.env = "ULW_JWKS_MAX_STALE_HOURS", .key = "auth.jwks_max_stale_hours"},
    ops::Setting{.env = "ULW_DEV_MODE", .key = "dev.mode"},
    // Not a setting: the kubelet sets it in every container, and it is read only to refuse
    // development settings there.
    ops::Setting{.env = "KUBERNETES_SERVICE_HOST", .key = ""},
    ops::Setting{.env = "JWT_ISSUER", .key = "auth.issuer"},
    ops::Setting{.env = "JWT_AUDIENCE", .key = "auth.audience"},
    ops::Setting{.env = "ULW_JWT_SUBJECT_CLAIM", .key = "auth.subject_claim"},
    ops::Setting{.env = "ULW_AUTH_COOKIE", .key = "auth.cookie"},
    ops::Setting{.env = "ULW_ALLOWED_ORIGINS", .key = "auth.allowed_origins"},
    ops::Setting{.env = "ULW_ALLOW_SAME_SITE", .key = "auth.allow_same_site"},
    ops::Setting{.env = "ULW_LOG_LEVEL", .key = "log.level"},
    // Read by the store's credential provider; here only to be checked for.
    ops::Setting{.env = "ULW_S3_ACCESS_KEY_ID", .key = "", .secret = true},
    ops::Setting{.env = "ULW_S3_SECRET_ACCESS_KEY", .key = "", .secret = true},
    // The stream service (ADR-0092).
    ops::Setting{.env = "LIVEKIT_API_URL", .key = "live.livekit_api_url"},
    ops::Setting{.env = "LIVEKIT_CLIENT_URL", .key = "live.livekit_client_url"},
    ops::Setting{.env = "LIVEKIT_API_KEY", .key = "live.livekit_api_key"},
    ops::Setting{.env = "LIVEKIT_API_SECRET", .key = "live.livekit_api_secret", .secret = true},
    ops::Setting{.env = "ULW_LIVE_PACKAGER_SRT", .key = "live.packager_srt"},
    ops::Setting{.env = "ULW_LIVE_PACKAGER", .key = "live.packager"},
    ops::Setting{.env = "ULW_LIVE_SEGMENT_SECONDS", .key = "live.segment_seconds"},
    ops::Setting{.env = "ULW_LIVE_MAX_STREAMS", .key = "live.max_streams"},
    ops::Setting{.env = "ULW_LIVE_START_WINDOW_SECONDS", .key = "live.start_window_seconds"},
    ops::Setting{.env = "ULW_LIVE_STREAMS_PER_USER_PER_HOUR",
                 .key = "live.streams_per_user_per_hour"},
    ops::Setting{.env = "ULW_LIVE_BROADCASTER_CLAIM", .key = "live.broadcaster_claim"},
    ops::Setting{.env = "ULW_LIVE_PACKAGER_BIN", .key = "live.packager_bin"},
    ops::Setting{.env = "ULW_LIVE_PACKAGER_SCRATCH_DIR", .key = "live.packager_scratch_dir"},
    ops::Setting{.env = "ULW_LIVE_PACKAGER_PORT", .key = "live.packager_port"},
    ops::Setting{.env = "ULW_LIVE_JOB_TEMPLATE", .key = "live.job_template"},
    ops::Setting{.env = "ULW_LIVE_PACKAGER_IMAGE_TAG", .key = "live.packager_image_tag"},
    ops::Setting{.env = "ULW_LIVE_PACKAGER_PULL_POLICY", .key = "live.packager_pull_policy"},
    ops::Setting{.env = "ULW_LIVE_PACKAGER_SECRET", .key = "live.packager_secret"},
    ops::Setting{.env = "ULW_K8S_API_URL", .key = "live.k8s_api_url"},
    ops::Setting{.env = "ULW_K8S_NAMESPACE", .key = "live.k8s_namespace"},
    ops::Setting{.env = "ULW_K8S_TOKEN_FILE", .key = "live.k8s_token_file"},
    ops::Setting{.env = "ULW_K8S_CA_FILE", .key = "live.k8s_ca_file"},
    // LiveKit's webhooks (ADR-0093).
    ops::Setting{.env = "ULW_LIVE_WEBHOOK_PORT", .key = "live.webhook_port"},
    ops::Setting{.env = "ULW_LIVE_PUBLISHER_GRACE_SECONDS", .key = "live.publisher_grace_seconds"},
    // Not a setting: passed on to a packager the gateway starts as a process.
    ops::Setting{.env = "PATH", .key = ""},
};

// S3 and R2 refuse a part under 5 MiB unless it is the last, and above 5 GiB.
constexpr std::uint64_t kMinChunk = std::uint64_t{5} << 20U;
constexpr std::uint64_t kMaxChunk = std::uint64_t{5} << 30U;
// Neither allows more than 10,000 parts, so the largest upload bounds the chunk from below:
// 50 GiB / 10,000 is 5.12 MiB, just above the store's own minimum.
constexpr std::uint64_t kMaxParts = 10'000;
constexpr std::uint64_t kDescriptorReserve = 64;
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
    auto source = ops::key_source(env);
    if (!source) {
        return error(source.error().variable, source.error().reason);
    }
    config.jwks_url = std::move(source->url);
    config.dev_jwks_file = std::move(source->file);
    // A week is past any outage anyone would wait out; an hour is short of a bad night.
    auto stale = number<std::uint32_t>(env, "ULW_JWKS_MAX_STALE_HOURS", 24, 1, 168);
    if (!stale) {
        return std::unexpected(std::move(stale.error()));
    }
    config.jwks_max_stale_hours = *stale;
    // key_source has refused anything but "0", "1" or empty.
    config.dev_mode = lookup(env, "ULW_DEV_MODE") == "1";
    auto issuer = required(env, "JWT_ISSUER");
    if (!issuer) {
        return std::unexpected(std::move(issuer.error()));
    }
    config.jwt_issuer = std::move(*issuer);
    auto rules =
        ops::token_rules(env, ops::KeySource{.url = config.jwks_url, .file = config.dev_jwks_file});
    if (!rules) {
        return error(rules.error().variable, rules.error().reason);
    }
    config.jwt_audience = std::move(rules->audience);
    config.jwt_subject_claim = std::move(rules->subject_claim);
    config.limits.auth_cookie = lookup(env, "ULW_AUTH_COOKIE").value_or("auth_token");
    if (const auto list = lookup(env, "ULW_ALLOWED_ORIGINS")) {
        auto origins = http::parse_origin_list(*list);
        if (!origins) {
            return error("ULW_ALLOWED_ORIGINS", "expected comma-separated scheme://host[:port]");
        }
        config.limits.allowed_origins = std::move(*origins);
    }
    if (const auto same_site = lookup(env, "ULW_ALLOW_SAME_SITE")) {
        if (*same_site != "0" && *same_site != "1") {
            return error("ULW_ALLOW_SAME_SITE", "expected 0 or 1");
        }
        config.limits.allow_same_site = *same_site == "1";
    }
    if (config.dev_jwks_file.empty()) {
        return {};
    }
    auto jwks = read_key_set(config.dev_jwks_file);
    if (!jwks) {
        return error("ULW_DEV_JWKS_FILE", "unreadable, or larger than 64 KiB");
    }
    const auto keys = infra::auth::Ed25519LocalVerifier::create(
        *jwks, {.issuer = config.jwt_issuer,
                .audience = config.jwt_audience,
                .subject_claim = config.jwt_subject_claim});
    if (!keys) {
        return error("ULW_DEV_JWKS_FILE", infra::auth::to_string(keys.error()));
    }
    config.dev_jwks = std::move(*jwks);
    return {};
}

std::expected<std::vector<net::IpNetwork>, ConfigError> parse_proxies(std::string_view text) {
    auto proxies = http::parse_trusted_proxies(text);
    if (!proxies) {
        return error("ULW_TRUSTED_PROXIES", proxies.error());
    }
    return std::move(*proxies);
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

// A small text file read once at start: the Job template, the pod's namespace.
std::optional<std::string> read_small_file(const std::string& path, std::size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string out;
    std::array<char, 4096> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.append(buf.data(), static_cast<std::size_t>(in.gcount()));
        if (out.size() > limit) {
            return std::nullopt;
        }
    }
    if (!in.eof()) {
        return std::nullopt;
    }
    return out;
}

constexpr std::string_view kServiceAccountNamespace =
    "/var/run/secrets/kubernetes.io/serviceaccount/namespace";
// A Job manifest is a few KiB; 64 KiB is not a template anyone meant.
constexpr std::size_t kMaxTemplate = std::size_t{64} * 1024;

// What a packager started as a process is given besides its stream: the gateway's own store
// and database, its scratch root, and where it listens (ADR-0092). Values, not the gateway's
// whole environment, which holds the LiveKit secret.
std::expected<void, ConfigError> load_process_runtime(const EnvLookup& env, const Config& config,
                                                      LiveConfig& live) {
    auto binary = required(env, "ULW_LIVE_PACKAGER_BIN");
    if (!binary) {
        return std::unexpected(std::move(binary.error()));
    }
    if (!binary->starts_with('/')) {
        return error("ULW_LIVE_PACKAGER_BIN", "not an absolute path");
    }
    auto scratch = required(env, "ULW_LIVE_PACKAGER_SCRATCH_DIR");
    if (!scratch) {
        return std::unexpected(std::move(scratch.error()));
    }
    if (!scratch->starts_with('/')) {
        return error("ULW_LIVE_PACKAGER_SCRATCH_DIR", "not an absolute path");
    }
    // One port for every packager: one stream at a time, which ULW_LIVE_MAX_STREAMS must say.
    const auto port = number<std::uint16_t>(env, "ULW_LIVE_PACKAGER_PORT", 9000, 1, 65'535);
    if (!port) {
        return std::unexpected(port.error());
    }
    if (live.settings.max_streams != 1) {
        return error("ULW_LIVE_MAX_STREAMS",
                     "must be 1 with ULW_LIVE_PACKAGER=process: every packager takes one port");
    }
    std::vector<std::string> lines;
    const auto add = [&lines](std::string_view name, std::string_view value) {
        lines.push_back(std::string(name) + "=" + std::string(value));
    };
    switch (config.storage) {
    case StorageBackend::R2:
        add("ULW_STORAGE", "r2");
        add("ULW_R2_ACCOUNT_ID", config.storage_location);
        break;
    case StorageBackend::Minio:
        add("ULW_STORAGE", "minio");
        add("ULW_S3_ENDPOINT", config.storage_location);
        break;
    case StorageBackend::Filesystem:
        add("ULW_STORAGE", "fs");
        add("ULW_FS_ROOT", config.storage_location);
        break;
    }
    if (config.storage != StorageBackend::Filesystem) {
        add("ULW_BUCKET", config.bucket);
        for (const std::string_view key : {"ULW_S3_ACCESS_KEY_ID", "ULW_S3_SECRET_ACCESS_KEY"}) {
            add(key, lookup(env, key).value_or(""));
        }
    }
    add("ULW_DATABASE_URL", config.database_url);
    add("ULW_SCRATCH_DIR", *scratch);
    add("ULW_LIVE_INGEST_PORT", std::to_string(*port));
    add("ULW_LIVE_SEGMENT_SECONDS", std::to_string(live.settings.segment.count()));
    add("ULW_LIVE_CALLER_WAIT_SECONDS", "60");
    if (const auto path = lookup(env, "PATH")) {
        add("PATH", *path);
    }
    live.packager_binary = std::move(*binary);
    live.packager_environment = std::move(lines);
    return {};
}

std::expected<void, ConfigError> load_kubernetes_runtime(const EnvLookup& env, const Config& config,
                                                         LiveConfig& live) {
    // A packager Job reaches the gateway's store over the network, never its directory.
    if (config.storage == StorageBackend::Filesystem) {
        return error("ULW_LIVE_PACKAGER", "kubernetes needs ULW_STORAGE r2 or minio, not fs");
    }
    auto path = required(env, "ULW_LIVE_JOB_TEMPLATE");
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    auto text = read_small_file(*path, kMaxTemplate);
    if (!text) {
        return error("ULW_LIVE_JOB_TEMPLATE", "unreadable, or larger than 64 KiB");
    }
    if (!text->contains("${ULW_STREAM_ID}")) {
        return error("ULW_LIVE_JOB_TEMPLATE", "names no ${ULW_STREAM_ID}");
    }
    auto tag = required(env, "ULW_LIVE_PACKAGER_IMAGE_TAG");
    if (!tag) {
        return std::unexpected(std::move(tag.error()));
    }
    live.job_template_file = std::move(*path);
    live.job_template = std::move(*text);
    live.image_tag = std::move(*tag);
    if (auto policy = lookup(env, "ULW_LIVE_PACKAGER_PULL_POLICY")) {
        if (*policy != "Always" && *policy != "IfNotPresent" && *policy != "Never") {
            return error("ULW_LIVE_PACKAGER_PULL_POLICY", "must be Always, IfNotPresent or Never");
        }
        live.pull_policy = std::move(*policy);
    }
    if (auto secret = lookup(env, "ULW_LIVE_PACKAGER_SECRET")) {
        live.packager_secret = std::move(*secret);
    }
    if (auto url = lookup(env, "ULW_K8S_API_URL")) {
        if (!url->starts_with("https://") && !url->starts_with("http://")) {
            return error("ULW_K8S_API_URL", "must be an http or https URL");
        }
        live.k8s_api_url = std::move(*url);
    }
    if (auto token = lookup(env, "ULW_K8S_TOKEN_FILE")) {
        live.k8s_token_file = std::move(*token);
    }
    if (auto ca = lookup(env, "ULW_K8S_CA_FILE")) {
        live.k8s_ca_file = std::move(*ca);
    }
    // The pod's own namespace, from its service account, unless named.
    auto ns = lookup(env, "ULW_K8S_NAMESPACE");
    if (!ns) {
        ns = read_small_file(std::string(kServiceAccountNamespace), 253);
    }
    while (ns && !ns->empty() && (ns->back() == '\n' || ns->back() == ' ')) {
        ns->pop_back();
    }
    if (!ns || ns->empty()) {
        return error("ULW_K8S_NAMESPACE", "not set, and the service account names none");
    }
    live.k8s_namespace = std::move(*ns);
    return {};
}

// LiveKit's webhooks (ADR-0093): a listener of their own, never the public one, and the grace
// a publisher that left has to come back.
std::expected<void, ConfigError> load_webhooks(const EnvLookup& env, Config& config) {
    LiveConfig& live = config.live;
    if (lookup(env, "ULW_LIVE_WEBHOOK_PORT")) {
        const auto port = number<std::uint16_t>(env, "ULW_LIVE_WEBHOOK_PORT", 0, 1, 65'535);
        if (!port) {
            return std::unexpected(port.error());
        }
        if (*port == config.port) {
            return error("ULW_LIVE_WEBHOOK_PORT",
                         "must not be ULW_LISTEN_PORT: webhooks are not served where the public "
                         "route sends requests");
        }
        live.webhook_port = *port;
    }
    // A full reconnect of LiveKit's client takes seconds; past five minutes a stream nobody
    // publishes is not one anybody is waiting for.
    const auto grace = number<std::uint32_t>(env, "ULW_LIVE_PUBLISHER_GRACE_SECONDS", 10, 1, 300);
    if (!grace) {
        return std::unexpected(grace.error());
    }
    live.watch.grace = core::Seconds{*grace};
    return {};
}

// How many streams, for how long, and who may start one.
std::expected<void, ConfigError> load_live_limits(const EnvLookup& env, LiveConfig& live) {
    // The packager's own bounds (ADR-0046).
    const auto segment = number<std::uint32_t>(env, "ULW_LIVE_SEGMENT_SECONDS", 2, 2, 10);
    if (!segment) {
        return std::unexpected(segment.error());
    }
    // At most what one sweep looks at, so that every unfinished stream is looked at each time.
    const auto streams = number<std::uint32_t>(
        env, "ULW_LIVE_MAX_STREAMS", 2, 1, static_cast<std::uint32_t>(LiveSettings{}.sweep_batch));
    if (!streams) {
        return std::unexpected(streams.error());
    }
    // A minute is one ticket; a day is a stream nobody meant.
    const auto window =
        number<std::uint32_t>(env, "ULW_LIVE_START_WINDOW_SECONDS", 120, 60, 86'400);
    if (!window) {
        return std::unexpected(window.error());
    }
    const auto per_user =
        number<std::uint32_t>(env, "ULW_LIVE_STREAMS_PER_USER_PER_HOUR", 6, 1, 1'000);
    if (!per_user) {
        return std::unexpected(per_user.error());
    }
    if (const auto claim = lookup(env, "ULW_LIVE_BROADCASTER_CLAIM")) {
        const std::size_t eq = claim->find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == claim->size()) {
            return error("ULW_LIVE_BROADCASTER_CLAIM", "expected <claim>=<value>");
        }
        live.broadcaster_claim = claim->substr(0, eq);
        live.broadcaster_value = claim->substr(eq + 1);
    }
    live.settings.segment = core::Seconds{*segment};
    live.settings.max_streams = *streams;
    live.settings.start_window = core::Seconds{*window};
    live.settings.streams_per_user_per_hour = *per_user;
    return {};
}

std::expected<void, ConfigError> load_live(const EnvLookup& env, Config& config) {
    LiveConfig& live = config.live;
    auto api = lookup(env, "LIVEKIT_API_URL");
    if (!api) {
        // Off: nothing else of it may be set, or a deployment would believe it publishes.
        for (const std::string_view name :
             {"LIVEKIT_CLIENT_URL", "ULW_LIVE_PACKAGER", "ULW_LIVE_PACKAGER_SRT",
              "ULW_LIVE_BROADCASTER_CLAIM", "ULW_LIVE_WEBHOOK_PORT"}) {
            if (lookup(env, name)) {
                return error(name, "set, but LIVEKIT_API_URL is not");
            }
        }
        return {};
    }
    if (!api->starts_with("http://") && !api->starts_with("https://")) {
        return error("LIVEKIT_API_URL", "must be an http or https URL");
    }
    auto client = required(env, "LIVEKIT_CLIENT_URL");
    if (!client) {
        return std::unexpected(std::move(client.error()));
    }
    if (!client->starts_with("ws://") && !client->starts_with("wss://")) {
        return error("LIVEKIT_CLIENT_URL", "must be a ws or wss URL");
    }
    auto key = required(env, "LIVEKIT_API_KEY");
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    auto secret = required(env, "LIVEKIT_API_SECRET");
    if (!secret) {
        return std::unexpected(std::move(secret.error()));
    }
    auto srt = required(env, "ULW_LIVE_PACKAGER_SRT");
    if (!srt) {
        return std::unexpected(std::move(srt.error()));
    }
    if (!srt->starts_with("srt://")) {
        return error("ULW_LIVE_PACKAGER_SRT", "must be srt://<host>:<port>");
    }
    if (auto r = load_live_limits(env, live); !r) {
        return r;
    }
    const std::string runtime = lookup(env, "ULW_LIVE_PACKAGER").value_or("");
    if (runtime == "process") {
        live.runtime = PackagerRuntime::Process;
        if (auto r = load_process_runtime(env, config, live); !r) {
            return r;
        }
    } else if (runtime == "kubernetes") {
        live.runtime = PackagerRuntime::Kubernetes;
        if (auto r = load_kubernetes_runtime(env, config, live); !r) {
            return r;
        }
    } else {
        return error("ULW_LIVE_PACKAGER", "expected process or kubernetes");
    }
    live.enabled = true;
    live.livekit_api_url = std::move(*api);
    live.livekit_client_url = std::move(*client);
    live.livekit_api_key = std::move(*key);
    live.livekit_api_secret = std::move(*secret);
    live.packager_srt = std::move(*srt);
    return {};
}

// ULW_LIVE_BROADCASTER_CLAIM as it was given.
std::string broadcaster_setting(const LiveConfig& live) {
    return live.broadcaster_claim.empty() ? std::string{}
                                          : live.broadcaster_claim + "=" + live.broadcaster_value;
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
    if (auto r = load_live(env, config); !r) {
        return std::unexpected(std::move(r.error()));
    }
    if (config.live.enabled) {
        if (auto r = load_webhooks(env, config); !r) {
            return std::unexpected(std::move(r.error()));
        }
    }
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

namespace {

// ULW_LIVE_WEBHOOK_PORT and ULW_LIVE_PUBLISHER_GRACE_SECONDS as logged: empty while the webhook
// listener is off.
std::pair<std::string, std::string> webhook_settings(const LiveConfig& live) {
    if (live.webhook_port == 0) {
        return {};
    }
    return {std::to_string(live.webhook_port),
            std::to_string(std::chrono::duration_cast<core::Seconds>(live.watch.grace).count())};
}

// One line per setting that has a value, secrets redacted, saying where each came from.
template <std::size_t N>
void log_values(const std::array<std::pair<std::string_view, std::string>, N>& values,
                const ops::Settings& layers, ops::Logger& log) {
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
}

// The stream service's settings, as logged: empty where live streams are off.
std::array<std::pair<std::string_view, std::string>, 21> live_values(const LiveConfig& live) {
    const auto live_value = [&live](std::string value) {
        return live.enabled ? std::move(value) : std::string{};
    };
    const bool process = live.enabled && live.runtime == PackagerRuntime::Process;
    const bool kubernetes = live.enabled && live.runtime == PackagerRuntime::Kubernetes;
    const auto k8s_value = [kubernetes](std::string value) {
        return kubernetes ? std::move(value) : std::string{};
    };
    const auto webhook_values = webhook_settings(live);
    return {{
        {"LIVEKIT_API_URL", live.livekit_api_url},
        {"LIVEKIT_CLIENT_URL", live.livekit_client_url},
        {"LIVEKIT_API_KEY", live.livekit_api_key},
        {"LIVEKIT_API_SECRET", live.livekit_api_secret},
        {"ULW_LIVE_PACKAGER_SRT", live.packager_srt},
        {"ULW_LIVE_PACKAGER", live_value(process ? "process" : "kubernetes")},
        {"ULW_LIVE_SEGMENT_SECONDS", live_value(std::to_string(live.settings.segment.count()))},
        {"ULW_LIVE_MAX_STREAMS", live_value(std::to_string(live.settings.max_streams))},
        {"ULW_LIVE_STREAMS_PER_USER_PER_HOUR",
         live_value(std::to_string(live.settings.streams_per_user_per_hour))},
        {"ULW_LIVE_BROADCASTER_CLAIM", broadcaster_setting(live)},
        {"ULW_LIVE_START_WINDOW_SECONDS",
         live_value(std::to_string(live.settings.start_window.count()))},
        {"ULW_LIVE_PACKAGER_BIN", live.packager_binary},
        {"ULW_LIVE_JOB_TEMPLATE", live.job_template_file},
        {"ULW_LIVE_PACKAGER_IMAGE_TAG", live.image_tag},
        {"ULW_LIVE_PACKAGER_PULL_POLICY", k8s_value(live.pull_policy)},
        {"ULW_LIVE_PACKAGER_SECRET", k8s_value(live.packager_secret)},
        {"ULW_K8S_API_URL", k8s_value(live.k8s_api_url)},
        {"ULW_K8S_NAMESPACE", live.k8s_namespace},
        {"ULW_K8S_TOKEN_FILE", k8s_value(live.k8s_token_file)},
        {"ULW_LIVE_WEBHOOK_PORT", webhook_values.first},
        {"ULW_LIVE_PUBLISHER_GRACE_SECONDS", webhook_values.second},
    }};
}

} // namespace

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
    std::string origins;
    for (const std::string& origin : config.limits.allowed_origins) {
        origins += (origins.empty() ? "" : ",") + origin;
    }
    const std::array<std::pair<std::string_view, std::string>, 33> values{{
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
        {"ULW_JWKS_MAX_STALE_HOURS",
         config.jwks_url.empty() ? "" : std::to_string(config.jwks_max_stale_hours)},
        {"ULW_DEV_JWKS_FILE", config.dev_jwks_file},
        {"ULW_DEV_MODE", config.dev_mode ? "1" : ""},
        {"JWT_ISSUER", config.jwt_issuer},
        {"JWT_AUDIENCE", config.jwt_audience},
        {"ULW_JWT_SUBJECT_CLAIM", config.jwt_subject_claim},
        {"ULW_AUTH_COOKIE", config.limits.auth_cookie},
        {"ULW_ALLOWED_ORIGINS", origins},
        {"ULW_ALLOW_SAME_SITE", config.limits.allow_same_site ? "1" : ""},
        {"ULW_LOG_LEVEL", std::string(ops::to_string(config.log_level))},
    }};
    log_values(values, layers, log);
    log_values(live_values(config.live), layers, log);
    for (const net::IpNetwork& block : config.limits.trusted_proxies) {
        if (block.prefix_length() < (block.is_v4() ? kWideV4Prefix : kWideV6Prefix)) {
            log.warn("a trusted proxy block this wide lets many peers name any client",
                     {{"prefix_length", block.prefix_length()}});
        }
    }
}

} // namespace gateway
