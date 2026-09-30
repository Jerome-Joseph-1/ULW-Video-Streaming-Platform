#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"
#include "net/ip_address.hpp"
#include "net/reactor_factory.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chat {

// ADR-0076's per-client limits, each unset meaning the service's default (chat::Limits).
struct ClientLimits {
    std::optional<std::size_t> max_connections_per_ip;
    std::optional<std::uint32_t> new_connections_per_ip_per_second;
    std::optional<std::size_t> max_sessions_per_user;
    // ULW_TRUSTED_PROXIES and ULW_TRUSTED_PROXY_HOPS, as the gateway's.
    std::vector<net::IpNetwork> trusted_proxies;
    std::size_t trusted_proxy_hops = 1;
};

struct Config {
    core::NodeId node;
    // Clients: WebSocket upgrades on /rt, and the health and metrics endpoints.
    std::uint16_t port = 9101;
    // The numeric host:port other chat nodes dial for this one's node channel, and the only
    // address the channel listens on. Never 0.0.0.0 or ::, and loopback only with
    // ULW_DEV_LOOPBACK_NODES=1, for a cluster on one host.
    std::string node_address;
    // ULW_NODE_SECRET: what every node proves it holds before the node channel carries anything
    // (ADR-0035). Never logged.
    std::string node_secret;
    net::ReactorKind reactor = net::ReactorKind::IoUring;
    std::string database_url;
    // Exactly one of the two: Askedin's JWKS, or a local key set for offline development.
    std::string jwks_url;
    std::string dev_jwks_file;
    std::string jwt_issuer;
    std::string jwt_audience;
    std::string auth_cookie;
    // Pages allowed to open a socket that authenticates with the cookie, as exact
    // "scheme://host[:port]" origins. Empty: the cookie is not accepted at all.
    std::vector<std::string> allowed_origins;
    // ULW_PRESENCE_GRACE_MS: how long a user whose last connection closed still shows online.
    // Unset: PresenceLimits::grace.
    std::optional<core::Millis> presence_grace;
    ClientLimits client_limits;
    // Who to become when started as root.
    std::string run_as_user;
    // Stay root when started as root with no run_as_user; otherwise that is refused.
    bool allow_root = false;
};

struct ConfigError {
    std::string variable;
    std::string reason;
};

// As the gateway's: an empty value counts as unset.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Everything comes from the environment. The node's name is ULW_NODE_ID, or else HOSTNAME,
// which Kubernetes sets to the pod's name.
[[nodiscard]] std::expected<Config, ConfigError> load_config(const EnvLookup& env);

} // namespace chat
