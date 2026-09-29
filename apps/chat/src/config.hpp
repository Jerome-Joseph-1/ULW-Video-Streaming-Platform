#pragma once

#include "core/models/ids.hpp"
#include "net/reactor_factory.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chat {

struct Config {
    core::NodeId node;
    // Clients: WebSocket upgrades on /rt, and the health and metrics endpoints.
    std::uint16_t port = 9101;
    // The numeric host:port other chat nodes dial for this one's node channel. The channel
    // listens on its port, on every interface.
    std::string node_address;
    std::uint16_t node_port = 0;
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
