#include "http/origin.hpp"

#include <algorithm>

namespace http {

namespace {

// Plain http carries the cookie in the clear, so only a page on this machine (a dev server) may
// be one; a browser writes these hosts as given.
[[nodiscard]] bool is_loopback(std::string_view host) noexcept {
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

// A port as a browser writes it: 1 to 65535 in decimal, without leading zeros. nullopt otherwise.
[[nodiscard]] std::optional<unsigned> parse_port(std::string_view port) noexcept {
    if (port.empty() || port.size() > 5 || port.front() == '0') {
        return std::nullopt;
    }
    unsigned value = 0;
    for (const char c : port) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + static_cast<unsigned>(c - '0');
    }
    if (value > 65535) {
        return std::nullopt;
    }
    return value;
}

} // namespace

bool is_origin(std::string_view origin) noexcept {
    // The scheme is the token before the first "://"; only https, and http for loopback, are taken.
    const std::size_t separator = origin.find("://");
    if (separator == std::string_view::npos) {
        return false;
    }
    const std::string_view scheme = origin.substr(0, separator);
    const std::string_view rest = origin.substr(separator + 3);
    unsigned default_port = 0;
    bool plain = false;
    if (scheme == "https") {
        default_port = 443;
    } else if (scheme == "http") {
        default_port = 80;
        plain = true;
    } else {
        return false;
    }
    // Printable ASCII only, lowercase, and nothing past the authority.
    if (rest.find_first_of("/?#@") != std::string_view::npos ||
        std::ranges::any_of(rest,
                            [](char c) { return c < '!' || c > '~' || (c >= 'A' && c <= 'Z'); })) {
        return false;
    }
    // host[:port], the host an IPv6 literal in brackets or a name without ':'.
    std::size_t host_end = rest.find(':');
    if (rest.starts_with('[')) {
        const std::size_t close = rest.find(']');
        if (close == std::string_view::npos || close == 1) {
            return false;
        }
        // An IPv6 literal (with an embedded IPv4 tail) holds hex digits, ':' and '.' only.
        if (!std::ranges::all_of(rest.substr(1, close - 1), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':' || c == '.';
            })) {
            return false;
        }
        host_end = close + 1;
    }
    host_end = std::min(host_end, rest.size());
    const std::string_view host = rest.substr(0, host_end);
    if (host.empty() || (plain && !is_loopback(host))) {
        return false;
    }
    if (host_end == rest.size()) {
        return true;
    }
    // A browser leaves the scheme's own port out of Origin, so a list naming it never matches.
    const auto port = parse_port(rest.substr(host_end + 1));
    return rest[host_end] == ':' && port && *port != default_port;
}

std::optional<std::vector<std::string>> parse_origin_list(std::string_view list) {
    std::vector<std::string> out;
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view origin = list.substr(0, comma);
        if (!is_origin(origin)) {
            return std::nullopt;
        }
        out.emplace_back(origin);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    return out;
}

bool origin_allowed(std::span<const std::string> allowed, std::string_view origin) noexcept {
    return std::ranges::find(allowed, origin) != allowed.end();
}

} // namespace http
