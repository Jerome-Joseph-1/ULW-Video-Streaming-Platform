#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

// A push subscription's endpoint is a URL the browser's push service chose, handed to us by a
// client: the server POSTs to it. Before it is stored, and again before every send, it must be
// https on port 443, of bounded length, with a host the operator allows (ULW_PUSH_HOSTS), and
// not an address literal. The address the host resolves to is checked when the connection opens
// (infra::curl::Request::public_only), so a name that resolves to a private address reaches
// nothing either.
namespace infra::webpush {

// The longest endpoint kept. The major services' are 150 to 500 characters (WNS's the longest).
inline constexpr std::size_t kMaxEndpointBytes = 2048;
// Hosts allowed when the operator names none: the push services of Chrome and the other
// Chromium browsers (FCM), Firefox (Mozilla autopush), Safari (Apple) and Edge (WNS).
inline constexpr std::string_view kDefaultPushHosts =
    "fcm.googleapis.com,updates.push.services.mozilla.com,web.push.apple.com,*.notify.windows.com";
// More entries than any real list, which names a handful.
inline constexpr std::size_t kMaxPushHosts = 64;

enum class EndpointError : std::uint8_t {
    // Longer than kMaxEndpointBytes.
    TooLong,
    // Not https://, or a port other than 443.
    NotHttps,
    // Not a URL this accepts: userinfo, a fragment, a character outside printable ASCII or one a
    // URL never carries, an empty or overlong label, no path.
    Malformed,
    // An IP address, not a name.
    AddressLiteral,
    // A host the operator's list does not name.
    HostNotAllowed,
};

// The operator's allowlist of push service hosts: exact names, or "*.suffix" for any name with
// at least one more label in front of the suffix. Compared in lower case.
class PushHosts {
public:
    // Comma-separated; blanks around entries are ignored. Refuses an empty list, an entry that is
    // not a host name (or a wildcard over one with at least two labels), and more than
    // kMaxPushHosts entries.
    [[nodiscard]] static std::expected<PushHosts, std::string> parse(std::string_view list);
    [[nodiscard]] static PushHosts defaults();

    [[nodiscard]] bool allows(std::string_view host) const noexcept;
    [[nodiscard]] const std::vector<std::string>& entries() const noexcept { return entries_; }

private:
    std::vector<std::string> entries_;
};

struct Endpoint {
    // As given.
    std::string url;
    // "https://<host>", the host in lower case: the VAPID token's audience.
    std::string origin;
};

// `any_port` lets a port other than 443 through, for a test push service on loopback
// (ULW_DEV_PUSH_ALLOW_PRIVATE); never in production.
[[nodiscard]] std::expected<Endpoint, EndpointError>
check_endpoint(std::string_view url, const PushHosts& hosts, bool any_port = false);

[[nodiscard]] std::string_view to_string(EndpointError error) noexcept;

} // namespace infra::webpush
