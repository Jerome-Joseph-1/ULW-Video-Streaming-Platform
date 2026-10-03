#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace core {

// The scheme and host of `scheme://host[:port][/...]`, a bracketed IPv6 host kept in its
// brackets; nullopt for anything else.
struct UrlParts {
    std::string_view scheme;
    std::string_view host;
};

[[nodiscard]] constexpr std::optional<UrlParts> split_url(std::string_view url) noexcept {
    const std::size_t colon = url.find("://");
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    const std::string_view scheme = url.substr(0, colon);
    std::string_view rest = url.substr(colon + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    std::string_view host;
    if (rest.starts_with('[')) {
        const std::size_t close = rest.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = rest.substr(0, close + 1);
    } else {
        host = rest.substr(0, rest.find(':'));
    }
    if (host.empty()) {
        return std::nullopt;
    }
    return UrlParts{.scheme = scheme, .host = host};
}

// A host whose traffic never leaves the machine: localhost, 127.0.0.0/8 or [::1].
[[nodiscard]] constexpr bool loopback_host(std::string_view host) noexcept {
    if (host == "localhost" || host == "[::1]") {
        return true;
    }
    // A dotted IPv4 address in 127.0.0.0/8, not a name that begins like one.
    std::size_t dots = 0;
    for (const char c : host) {
        if (c == '.') {
            ++dots;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return dots == 3 && host.starts_with("127.");
}

// A host only a cluster's own DNS resolves: a single label (a Service in the pod's namespace)
// or a name under .svc or .svc.cluster.local.
[[nodiscard]] constexpr bool cluster_host(std::string_view host) noexcept {
    return !host.contains('.') || host.ends_with(".svc") || host.ends_with(".svc.cluster.local");
}

// Whether `url` names `secure` as its scheme, or `plain` to a host `plain_ok` admits: a plain
// scheme is only for traffic that never crosses a network an attacker shares.
template <class PlainOk>
[[nodiscard]] constexpr bool secure_url(std::string_view url, std::string_view secure,
                                        std::string_view plain, PlainOk plain_ok) noexcept {
    const auto parts = split_url(url);
    return parts && (parts->scheme == secure || (parts->scheme == plain && plain_ok(parts->host)));
}

} // namespace core
