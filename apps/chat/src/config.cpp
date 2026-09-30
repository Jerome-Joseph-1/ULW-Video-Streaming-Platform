#include "config.hpp"

#include "core/util/parse.hpp"
#include "http/client_limits.hpp"
#include "net/socket.hpp"
#include "rt/room_router.hpp"

#include "ops/root.hpp"

#include <algorithm>
#include <utility>

namespace chat {

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

// A browser sends Origin as scheme://host[:port], lowercase, with no path; anything else could
// never match one.
bool is_origin(std::string_view origin) {
    std::string_view rest;
    if (origin.starts_with("https://")) {
        rest = origin.substr(8);
    } else if (origin.starts_with("http://")) {
        rest = origin.substr(7);
    } else {
        return false;
    }
    return !rest.empty() && rest.find_first_of("/?#@ ") == std::string_view::npos &&
           std::ranges::none_of(rest, [](char c) { return c >= 'A' && c <= 'Z'; });
}

std::expected<std::vector<std::string>, ConfigError> origins(const EnvLookup& env) {
    std::vector<std::string> out;
    const auto list = lookup(env, "ULW_ALLOWED_ORIGINS");
    if (!list) {
        return out;
    }
    std::string_view rest = *list;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view origin = rest.substr(0, comma);
        if (!is_origin(origin)) {
            return error("ULW_ALLOWED_ORIGINS", "expected comma-separated scheme://host[:port]");
        }
        out.emplace_back(origin);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    return out;
}

std::expected<std::string, ConfigError> checked_node_address(const EnvLookup& env,
                                                             std::uint16_t client_port) {
    auto node_address = lookup(env, "ULW_NODE_ADDRESS");
    if (!node_address) {
        return error("ULW_NODE_ADDRESS", "not set");
    }
    // Other nodes connect to exactly this, and must not have to look a name up to do it.
    if (!net::is_numeric_endpoint(*node_address)) {
        return error("ULW_NODE_ADDRESS", "expected a numeric ip:port or [ipv6]:port");
    }
    // Published for other nodes to dial: an address naming no host, or only this one, would
    // send them nowhere or to themselves. Loopback is for a test cluster on one host, and is
    // allowed only when asked for by name.
    switch (net::endpoint_scope(*node_address).value_or(net::EndpointScope::Unspecified)) {
    case net::EndpointScope::Unspecified:
        return error("ULW_NODE_ADDRESS", "names no host (0.0.0.0 or ::); give this pod's address");
    case net::EndpointScope::Loopback:
        if (lookup(env, "ULW_DEV_LOOPBACK_NODES") != "1") {
            return error("ULW_NODE_ADDRESS", "is loopback, which other nodes cannot reach; set "
                                             "ULW_DEV_LOOPBACK_NODES=1 for a cluster on one host");
        }
        break;
    case net::EndpointScope::Routable:
        break;
    }
    // Validated just above, so the port parses.
    const std::uint16_t node_port =
        core::parse_integer<std::uint16_t>(
            std::string_view{*node_address}.substr(node_address->rfind(':') + 1))
            .value_or(0);
    if (node_port == client_port) {
        return error("ULW_NODE_ADDRESS", "uses ULW_LISTEN_PORT, which clients already take");
    }
    return std::move(*node_address);
}

std::expected<std::optional<core::Millis>, ConfigError> presence_grace(const EnvLookup& env) {
    const auto text = lookup(env, "ULW_PRESENCE_GRACE_MS");
    if (!text) {
        return std::nullopt;
    }
    // Ten minutes is far past any reconnect; beyond it, a user who left would be shown online
    // for longer than anyone would call a grace.
    constexpr std::uint32_t kMaxGraceMs = 600'000;
    const auto value = core::parse_integer<std::uint32_t>(*text);
    if (!value || *value > kMaxGraceMs) {
        return error("ULW_PRESENCE_GRACE_MS", "expected milliseconds, 0 to 600000");
    }
    return core::Millis{*value};
}

template <class T>
std::expected<std::optional<T>, ConfigError> bounded(const EnvLookup& env, std::string_view name,
                                                     T min, T max) {
    const auto text = lookup(env, name);
    if (!text) {
        return std::nullopt;
    }
    const auto value = core::parse_integer<T>(*text);
    if (!value || *value < min || *value > max) {
        return error(name, "not an integer in range");
    }
    return *value;
}

std::expected<ClientLimits, ConfigError> client_limits(const EnvLookup& env) {
    // No more than the node's 1280 connections (chat::Limits): past it a per-client limit would
    // never be reached.
    constexpr std::size_t kMaxConnections = 1280;
    constexpr std::size_t kMaxProxyHops = 16;
    ClientLimits out;
    const auto per_ip = bounded<std::size_t>(env, "ULW_MAX_CONNECTIONS_PER_IP", 1, kMaxConnections);
    if (!per_ip) {
        return std::unexpected(per_ip.error());
    }
    out.max_connections_per_ip = *per_ip;
    const auto rate =
        bounded<std::uint32_t>(env, "ULW_NEW_CONNECTIONS_PER_IP_PER_SECOND", 1, 65'536);
    if (!rate) {
        return std::unexpected(rate.error());
    }
    out.new_connections_per_ip_per_second = *rate;
    const auto per_user =
        bounded<std::size_t>(env, "ULW_MAX_SESSIONS_PER_USER", 1, kMaxConnections);
    if (!per_user) {
        return std::unexpected(per_user.error());
    }
    out.max_sessions_per_user = *per_user;
    if (const auto text = lookup(env, "ULW_TRUSTED_PROXIES")) {
        auto proxies = http::parse_trusted_proxies(*text);
        if (!proxies) {
            return error("ULW_TRUSTED_PROXIES", proxies.error());
        }
        out.trusted_proxies = std::move(*proxies);
    }
    const auto hops = bounded<std::size_t>(env, "ULW_TRUSTED_PROXY_HOPS", 1, kMaxProxyHops);
    if (!hops) {
        return std::unexpected(hops.error());
    }
    out.trusted_proxy_hops = hops->value_or(1);
    return out;
}

} // namespace

std::expected<Config, ConfigError> load_config(const EnvLookup& env) {
    const char* node_variable = "ULW_NODE_ID";
    auto node_name = lookup(env, node_variable);
    if (!node_name) {
        node_variable = "HOSTNAME";
        node_name = lookup(env, node_variable);
    }
    if (!node_name) {
        return error("ULW_NODE_ID", "not set, and neither is HOSTNAME");
    }
    const auto node = core::NodeId::parse(*node_name);
    if (!node) {
        return error(node_variable, "not a node name: [a-z0-9-], at most 63 characters");
    }

    std::uint16_t port = 9101;
    if (const auto text = lookup(env, "ULW_LISTEN_PORT")) {
        // Port 0 would bind an ephemeral port nobody can be told about.
        const auto value = core::parse_integer<std::uint16_t>(*text);
        if (!value || *value == 0) {
            return error("ULW_LISTEN_PORT", "not an integer in range");
        }
        port = *value;
    }

    auto node_address = checked_node_address(env, port);
    if (!node_address) {
        return std::unexpected(node_address.error());
    }

    auto node_secret = lookup(env, "ULW_NODE_SECRET");
    if (!node_secret) {
        return error("ULW_NODE_SECRET", "not set");
    }
    if (node_secret->size() < rt::kMinNodeSecretBytes) {
        return error("ULW_NODE_SECRET", "shorter than 32 bytes");
    }

    net::ReactorKind reactor = net::ReactorKind::IoUring;
    if (const auto name = lookup(env, "ULW_REACTOR")) {
        const auto kind = net::parse_reactor_kind(*name);
        if (!kind) {
            return error("ULW_REACTOR", "expected io_uring or epoll");
        }
        reactor = *kind;
    }

    auto database = lookup(env, "ULW_DATABASE_URL");
    if (!database) {
        return error("ULW_DATABASE_URL", "not set");
    }

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
    auto issuer = lookup(env, "JWT_ISSUER");
    if (!issuer) {
        return error("JWT_ISSUER", "not set");
    }
    auto allowed = origins(env);
    if (!allowed) {
        return std::unexpected(std::move(allowed.error()));
    }
    auto grace = presence_grace(env);
    if (!grace) {
        return std::unexpected(std::move(grace.error()));
    }
    auto limits = client_limits(env);
    if (!limits) {
        return std::unexpected(std::move(limits.error()));
    }
    const auto allow_root = ops::parse_allow_root(lookup(env, "ULW_ALLOW_ROOT"));
    if (!allow_root) {
        return error("ULW_ALLOW_ROOT", "expected 0 or 1");
    }

    return Config{.node = *node,
                  .port = port,
                  .node_address = std::move(*node_address),
                  .node_secret = std::move(*node_secret),
                  .reactor = reactor,
                  .database_url = std::move(*database),
                  .jwks_url = std::move(url).value_or(""),
                  .dev_jwks_file = std::move(file).value_or(""),
                  .jwt_issuer = std::move(*issuer),
                  .jwt_audience = lookup(env, "JWT_AUDIENCE").value_or("askedin-platform"),
                  .auth_cookie = lookup(env, "ULW_AUTH_COOKIE").value_or("auth_token"),
                  .allowed_origins = std::move(*allowed),
                  .presence_grace = *grace,
                  .client_limits = std::move(*limits),
                  .run_as_user = lookup(env, "ULW_RUN_AS_USER").value_or(""),
                  .allow_root = *allow_root};
}

} // namespace chat
