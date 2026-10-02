#include "http/origin.hpp"

#include <algorithm>

namespace http {

namespace {

// Plain http carries the cookie in the clear, so only a page on this machine (a dev server) may
// be one; a browser writes these hosts as given.
[[nodiscard]] bool is_loopback(std::string_view host) noexcept {
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

} // namespace

bool is_origin(std::string_view origin) noexcept {
    std::string_view rest;
    std::string_view default_port;
    bool plain = false;
    if (origin.starts_with("https://")) {
        rest = origin.substr(8);
        default_port = "443";
    } else if (origin.starts_with("http://")) {
        rest = origin.substr(7);
        default_port = "80";
        plain = true;
    } else {
        return false;
    }
    if (rest.find_first_of("/?#@ ") != std::string_view::npos ||
        std::ranges::any_of(rest, [](char c) { return c >= 'A' && c <= 'Z'; })) {
        return false;
    }
    // host[:port], the host an IPv6 literal in brackets or a name without ':'.
    std::size_t host_end = rest.find(':');
    if (rest.starts_with('[')) {
        const std::size_t close = rest.find(']');
        if (close == std::string_view::npos) {
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
    const std::string_view port = rest.substr(host_end + 1);
    return rest[host_end] == ':' && !port.empty() && port != default_port;
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
