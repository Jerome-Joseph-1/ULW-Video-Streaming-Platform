#pragma once

#include "core/util/time.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::postgres {

enum class HostForm : std::uint8_t { Socket, Numeric, Name };

[[nodiscard]] HostForm classify_host(std::string_view host) noexcept;

// The settings of a connection string that decide where libpq connects, as libpq parses them.
// Variables such as PGHOST are not consulted: a host set only in the environment is resolved
// by libpq itself.
struct ConnTarget {
    std::vector<std::string> hosts;
    // Empty, one for every host, or one per host.
    std::vector<std::string> ports;
    bool has_hostaddr = false;
    bool has_service = false;
    std::string options;
};

[[nodiscard]] std::expected<ConnTarget, std::string> parse_conninfo(const std::string& conninfo);

// The host, hostaddr and port lists for PQconnectStartParams, comma-joined.
struct Endpoints {
    std::string host;
    std::string hostaddr;
    std::string port;
};

// One entry per address: a host name expands into every address `addresses` holds for it, in
// resolver order, which is the order libpq itself would have tried them. Sockets and numeric
// hosts keep an empty hostaddr, which tells libpq to use the host as given. `addresses` is
// indexed like target.hosts. nullopt when a name has no address or the port list does not
// match the hosts.
[[nodiscard]] std::optional<Endpoints>
expand_endpoints(const ConnTarget& target, std::span<const std::vector<std::string>> addresses);

// Client-side keepalive defaults, placed before the connection string so that it may override
// them. With the server-side twin in session_options, a peer that vanished without closing the
// socket is noticed after 10 s idle and 3 unanswered probes 5 s apart, 25 s in all, instead of
// the kernel's two hours.
inline constexpr std::array<std::pair<const char*, const char*>, 3> kKeepalives{{
    {"keepalives_idle", "10"},
    {"keepalives_interval", "5"},
    {"keepalives_count", "3"},
}};

// The `options` a session connects with: ours first, then the connection string's own `options`
// (`own`), which therefore win. A zero statement_timeout leaves statements and transactions
// unbounded.
[[nodiscard]] std::string session_options(core::Millis statement_timeout, std::string_view own);

// libpq resolves host names with a blocking getaddrinfo inside PQconnectStart. The reactor's
// sessions resolve beforehand on the offload pool and hand libpq addresses instead.
class ConnectPlan {
public:
    // Refuses a string that does not parse, and one that would leave libpq to find the host
    // itself, with a blocking lookup nothing here can see coming: a `service`, whose file may
    // name the host, or no host at all, which falls back on PGHOST.
    [[nodiscard]] static std::expected<ConnectPlan, std::string>
    create(std::string conninfo, std::string application_name, core::Millis statement_timeout);

    [[nodiscard]] bool needs_lookup() const noexcept { return lookup_; }
    // Blocks on DNS: offload threads only.
    [[nodiscard]] std::optional<Endpoints> resolve() const;

    // NULL-terminated arrays for PQconnectStartParams, pointing into this plan and `endpoints`.
    struct Arrays {
        std::vector<const char*> keywords;
        std::vector<const char*> values;
    };
    [[nodiscard]] Arrays arrays(const Endpoints* endpoints) const;

private:
    ConnectPlan(std::string conninfo, std::string application_name, ConnTarget target,
                core::Millis statement_timeout);

    std::string conninfo_;
    std::string application_name_;
    std::string options_;
    ConnTarget target_;
    bool lookup_ = false;
};

} // namespace infra::postgres
