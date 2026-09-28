#pragma once

#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// libpq resolves host names with a blocking getaddrinfo inside PQconnectStart. The reactor's
// sessions resolve beforehand on the offload pool and hand libpq addresses instead.
class ConnectPlan {
public:
    ConnectPlan(std::string conninfo, std::string application_name, core::Millis statement_timeout);

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
    std::string conninfo_;
    std::string application_name_;
    std::string options_;
    ConnTarget target_;
    bool lookup_ = false;
};

} // namespace infra::postgres
