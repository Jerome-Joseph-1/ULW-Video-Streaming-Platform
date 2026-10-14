#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/media.hpp"
#include "core/ports/turn.hpp"
#include "core/util/time.hpp"
#include "infra/curl/multi.hpp"
#include "net/reactor.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// An external TURN service in place of the cluster's own relay (ADR-0109): the credentials a
// client needs for it, issued per ticket, and the media port that hands them out with every
// ticket.
namespace infra::turn {

enum class Provider : std::uint8_t {
    // Any TURN server that checks the TURN REST API's time-limited credentials against a secret
    // it shares with us: coturn (use-auth-secret), eturnal, STUNner's ephemeral auth. The
    // username is "<expiry>:<name>", the password its HMAC-SHA1 under the secret, in base64.
    Hmac,
    // Cloudflare Realtime TURN: a TURN key's id and API token, from which its API generates each
    // client's ICE servers.
    Cloudflare,
};

struct Config {
    Provider provider = Provider::Hmac;
    // Hmac: the URIs the credentials are good for, as clients use them ("turn:relay.example.com:
    // 3478?transport=udp", "turns:relay.example.com:5349?transport=tcp").
    std::vector<std::string> urls;
    // Hmac: the secret shared with the TURN server. Never logged.
    std::string shared_secret;
    // Cloudflare: the TURN key's id and its API token. The token is never logged.
    std::string key_id;
    std::string api_token;
    // Cloudflare: where its API answers, its own by default; a test points it at a fake.
    std::string api_url = "https://rtc.live.cloudflare.com";
    // How long a credential admits its client. A client cannot renew one mid-call: it asks for
    // a new ticket, which carries fresh ones.
    core::Seconds ttl{14'400};
};

struct ConfigError {
    std::string variable;
    std::string reason;
};

using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// The external TURN settings, read the same way by every service that issues tickets:
// ULW_TURN_MODE (self-hosted, the default, or external), ULW_TURN_PROVIDER (hmac or cloudflare),
// ULW_TURN_URLS and ULW_TURN_SHARED_SECRET for hmac, ULW_TURN_KEY_ID, ULW_TURN_KEY_API_TOKEN and
// optionally ULW_TURN_API_URL for cloudflare, and ULW_TURN_TTL_SECONDS. nullopt in self-hosted
// mode, where no other ULW_TURN_* may be set: a deployment that half-configured external TURN
// would otherwise believe its media avoids the cluster's relay. An empty value is unset.
[[nodiscard]] std::expected<std::optional<Config>, ConfigError> load_config(const EnvLookup& env);

// For a startup log line: "self-hosted", "external hmac" or "external cloudflare". Never a
// credential.
[[nodiscard]] std::string_view describe(const std::optional<Config>& config) noexcept;

// `"ice_servers":[...]`, each server as WebRTC's RTCIceServer names its members (urls, and
// username and credential when it has them), so a client passes the array to its peer
// connection as it is. Nothing when `servers` is empty: the media server names its own relay.
void append_ice_servers(std::string& out, std::span<const core::ports::IceServer> servers);

// Every variable load_config reads, for a service that refuses settings it does not know.
inline constexpr std::array<std::string_view, 8> kVariables{
    "ULW_TURN_MODE",   "ULW_TURN_PROVIDER",      "ULW_TURN_URLS",    "ULW_TURN_SHARED_SECRET",
    "ULW_TURN_KEY_ID", "ULW_TURN_KEY_API_TOKEN", "ULW_TURN_API_URL", "ULW_TURN_TTL_SECONDS"};

// The provider's issuer, on `reactor`; Cloudflare's calls its API through `multi`. All three
// must outlive it.
[[nodiscard]] std::unique_ptr<core::ports::ITurnCredentials>
make_credentials(net::IReactor& reactor, curl::Multi& multi, const core::ports::IClock& clock,
                 Config config);

// `inner`, whose every ticket also carries the ICE servers `turn` issues for its participant,
// after the room admitted it. A ticket whose credentials could not be issued fails as the
// issuer did. `turn` must outlive the result.
[[nodiscard]] std::unique_ptr<core::ports::ISfu>
make_relayed_sfu(std::unique_ptr<core::ports::ISfu> inner, core::ports::ITurnCredentials& turn);

namespace detail {

// "<expiry>:<name>" and its password, as the TURN REST API draft and coturn define them.
[[nodiscard]] core::ports::IceServer hmac_server(const std::vector<std::string>& urls,
                                                 std::string_view secret, std::string_view name,
                                                 std::int64_t expiry_unix);

// The ICE servers in Cloudflare's answer to generate-ice-servers, without the URIs on port 53,
// which browsers may block and then wait on; nullopt when the answer is not that shape.
[[nodiscard]] std::optional<std::vector<core::ports::IceServer>>
parse_cloudflare(std::string_view body);

} // namespace detail

} // namespace infra::turn
