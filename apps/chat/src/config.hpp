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
    // Unset: four times the per-address cap.
    std::optional<std::size_t> max_connections_per_ip_block;
    std::optional<std::uint32_t> new_connections_per_ip_per_second;
    std::optional<std::size_t> max_sessions_per_user;
    // ULW_TRUSTED_PROXIES and ULW_TRUSTED_PROXY_HOPS, as the gateway's.
    std::vector<net::IpNetwork> trusted_proxies;
    std::size_t trusted_proxy_hops = 1;
};

// Where calls go (ADR-0050): LiveKit's server API, the URL clients reach it on, and the key pair
// tickets are signed with, as the call suite's harness names them. Never logged.
struct CallsConfig {
    // LIVEKIT_API_URL: "http://livekit:7880"; the node's own RoomService calls.
    std::string api_url;
    // LIVEKIT_CLIENT_URL: "wss://<media host>"; what every ticket names.
    std::string client_url;
    std::string api_key;
    std::string api_secret;
};

// Chat's service API (ADR-0096): a listener of its own for the operator's backend, which manages
// member lists with the identity provider's tokens that carry the service claim.
struct ServiceApiConfig {
    // ULW_SERVICE_PORT: never the client port or the node channel's.
    std::uint16_t port = 0;
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
    // Exactly one of the two: the identity provider's JWKS, or a local key set for offline
    // development.
    std::string jwks_url;
    // ULW_JWKS_MAX_STALE_HOURS: how long keys stay trusted while every refetch fails.
    std::uint32_t jwks_max_stale_hours = 24;
    std::string dev_jwks_file;
    std::string jwt_issuer;
    // Required with jwks_url; ops::kDevAudience by default with a local key set.
    std::string jwt_audience;
    // ULW_JWT_SUBJECT_CLAIM: the claim that names the user, `sub` by default.
    std::string jwt_subject_claim = "sub";
    // ULW_SERVICE_CLAIM (default `scope`) and ULW_SERVICE_SCOPE: which tokens are the operator's
    // backend (infra/auth/service_claim.hpp). No value: none is.
    std::string service_claim = "scope";
    std::string service_value;
    // ULW_SERVICE_CLIENT_ID: the client the service's token must name (azp or client_id), if
    // any.
    std::string service_client_id;
    std::string auth_cookie;
    // Pages allowed to open a socket that authenticates with the cookie, as exact
    // "scheme://host[:port]" origins. Empty: the cookie is not accepted at all.
    std::vector<std::string> allowed_origins;
    // ULW_PRESENCE_GRACE_MS: how long a user whose last connection closed still shows online.
    // Unset: PresenceLimits::grace.
    std::optional<core::Millis> presence_grace;
    // ULW_CALL_RING_TIMEOUT_MS: how long a call rings with nobody answering (ADR-0091). Unset:
    // RingLimits::ring_timeout.
    std::optional<core::Millis> ring_timeout;
    // ULW_CALL_GROUP_PARTICIPANTS: devices in a group chat's call, 3 to 16 (ADR-0095). Unset:
    // CallLimits::group_participants.
    std::optional<std::uint16_t> group_participants;
    ClientLimits client_limits;
    // Unset when LIVEKIT_API_KEY is: calls are not configured, and a call is answered
    // calls_disabled. With the key, the other three LIVEKIT_ variables are required.
    std::optional<CallsConfig> calls;
    // Unset when ULW_SERVICE_PORT is: no service API.
    std::optional<ServiceApiConfig> service_api;
    // Who to become when started as root.
    std::string run_as_user;
    // Stay root when started as root with no run_as_user; otherwise that is refused.
    bool allow_root = false;
    // ULW_CHAT_SELF_SERVICE, `on` or `off` (the default): whether users open direct chats,
    // create groups and add members over the room socket themselves (ADR-0096). Off, only the
    // service API does; leaving and an admin's removal stay with users either way.
    bool self_service = false;
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

// The prefix lengths of the trusted proxy blocks wider than an IPv4 /8 or an IPv6 /32, as the
// gateway warns of: rarely one's own proxies, and they let many peers name any client. Started
// with, and logged as a warning.
[[nodiscard]] std::vector<unsigned> wide_trusted_proxies(const ClientLimits& limits);

} // namespace chat
