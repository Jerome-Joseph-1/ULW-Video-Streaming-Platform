#include "config.hpp"

#include "core/util/parse.hpp"
#include "http/client_limits.hpp"
#include "http/origin.hpp"
#include "net/socket.hpp"
#include "rt/room_router.hpp"

#include "call.hpp"
#include "ops/dev_only.hpp"
#include "ops/root.hpp"

#include <algorithm>
#include <array>
#include <string>
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

std::expected<std::vector<std::string>, ConfigError> origins(const EnvLookup& env) {
    const auto list = lookup(env, "ULW_ALLOWED_ORIGINS");
    if (!list) {
        return std::vector<std::string>{};
    }
    auto out = http::parse_origin_list(*list);
    if (!out) {
        return error("ULW_ALLOWED_ORIGINS", "expected comma-separated scheme://host[:port]");
    }
    return std::move(*out);
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

std::expected<std::optional<core::Millis>, ConfigError> ring_timeout(const EnvLookup& env) {
    const auto text = lookup(env, "ULW_CALL_RING_TIMEOUT_MS");
    if (!text) {
        return std::nullopt;
    }
    // A second at least, for a ring anyone could answer (and the tests that wait for one to run
    // out); five minutes at most, past which nobody is still waiting for an answer.
    constexpr std::uint32_t kMinRingMs = 1'000;
    constexpr std::uint32_t kMaxRingMs = 300'000;
    const auto value = core::parse_integer<std::uint32_t>(*text);
    if (!value || *value < kMinRingMs || *value > kMaxRingMs) {
        return error("ULW_CALL_RING_TIMEOUT_MS", "expected milliseconds, 1000 to 300000");
    }
    return core::Millis{*value};
}

std::expected<std::uint32_t, ConfigError> jwks_max_stale_hours(const EnvLookup& env) {
    const auto text = lookup(env, "ULW_JWKS_MAX_STALE_HOURS");
    if (!text) {
        return 24;
    }
    // A week is past any outage anyone would wait out; an hour is short of a bad night.
    const auto value = core::parse_integer<std::uint32_t>(*text);
    if (!value || *value < 1 || *value > 168) {
        return error("ULW_JWKS_MAX_STALE_HOURS", "expected hours, 1 to 168");
    }
    return *value;
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
    // The /48 of IPv6 direct peers (ADR-0076). Below the per-address cap it caps a single /64
    // too, which a deployment may want; the range is all that is checked.
    const auto per_block =
        bounded<std::size_t>(env, "ULW_MAX_CONNECTIONS_PER_IP_BLOCK", 1, kMaxConnections);
    if (!per_block) {
        return std::unexpected(per_block.error());
    }
    out.max_connections_per_ip_block = *per_block;
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
    // As the gateway's: hops count proxies, and there are none to count.
    if (hops->has_value() && out.trusted_proxies.empty()) {
        return error("ULW_TRUSTED_PROXY_HOPS", "set, but ULW_TRUSTED_PROXIES is not");
    }
    out.trusted_proxy_hops = hops->value_or(1);
    return out;
}

// Optional as a whole, all or nothing once the key is given: the key is what a deployment
// without LiveKit leaves out (its secret does not exist), so the URLs alone, which an overlay may
// set for every environment, configure nothing. The adapter checks the values (make_sfu).
std::expected<std::optional<CallsConfig>, ConfigError> calls_config(const EnvLookup& env) {
    auto key = lookup(env, "LIVEKIT_API_KEY");
    if (!key) {
        return std::nullopt;
    }
    CallsConfig out{.api_url = {}, .client_url = {}, .api_key = std::move(*key), .api_secret = {}};
    const std::array<std::pair<std::string_view, std::string*>, 3> required{{
        {"LIVEKIT_API_URL", &out.api_url},
        {"LIVEKIT_CLIENT_URL", &out.client_url},
        {"LIVEKIT_API_SECRET", &out.api_secret},
    }};
    for (const auto& [name, into] : required) {
        auto value = lookup(env, name);
        if (!value) {
            return error(name, "not set, but LIVEKIT_API_KEY is");
        }
        *into = std::move(*value);
    }
    return out;
}

} // namespace

std::vector<unsigned> wide_trusted_proxies(const ClientLimits& limits) {
    // As the gateway's: a /8 of IPv4 is 16 million addresses, and a /32 of IPv6 a whole
    // provider's allocation.
    constexpr unsigned kWideV4Prefix = 8;
    constexpr unsigned kWideV6Prefix = 32;
    std::vector<unsigned> wide;
    for (const net::IpNetwork& block : limits.trusted_proxies) {
        if (block.prefix_length() < (block.is_v4() ? kWideV4Prefix : kWideV6Prefix)) {
            wide.push_back(block.prefix_length());
        }
    }
    return wide;
}

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

    auto keys = ops::key_source(env);
    if (!keys) {
        return error(keys.error().variable, keys.error().reason);
    }
    const auto max_stale_hours = jwks_max_stale_hours(env);
    if (!max_stale_hours) {
        return std::unexpected(max_stale_hours.error());
    }
    auto issuer = lookup(env, "JWT_ISSUER");
    if (!issuer) {
        return error("JWT_ISSUER", "not set");
    }
    auto rules = ops::token_rules(env, *keys);
    if (!rules) {
        return error(rules.error().variable, rules.error().reason);
    }
    auto allowed = origins(env);
    if (!allowed) {
        return std::unexpected(std::move(allowed.error()));
    }
    auto grace = presence_grace(env);
    if (!grace) {
        return std::unexpected(std::move(grace.error()));
    }
    auto ring = ring_timeout(env);
    if (!ring) {
        return std::unexpected(std::move(ring.error()));
    }
    auto group = bounded<std::uint16_t>(env, "ULW_CALL_GROUP_PARTICIPANTS", kMinGroupParticipants,
                                        kMaxGroupParticipants);
    if (!group) {
        return std::unexpected(std::move(group.error()));
    }
    auto limits = client_limits(env);
    if (!limits) {
        return std::unexpected(std::move(limits.error()));
    }
    auto calls = calls_config(env);
    if (!calls) {
        return std::unexpected(std::move(calls.error()));
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
                  .jwks_url = std::move(keys->url),
                  .jwks_max_stale_hours = *max_stale_hours,
                  .dev_jwks_file = std::move(keys->file),
                  .jwt_issuer = std::move(*issuer),
                  .jwt_audience = std::move(rules->audience),
                  .jwt_subject_claim = std::move(rules->subject_claim),
                  .auth_cookie = lookup(env, "ULW_AUTH_COOKIE").value_or("auth_token"),
                  .allowed_origins = std::move(*allowed),
                  .presence_grace = *grace,
                  .ring_timeout = *ring,
                  .group_participants = *group,
                  .client_limits = std::move(*limits),
                  .calls = std::move(*calls),
                  .run_as_user = lookup(env, "ULW_RUN_AS_USER").value_or(""),
                  .allow_root = *allow_root};
}

} // namespace chat
