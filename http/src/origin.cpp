#include "http/origin.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>

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

// The pieces of a run of ':'-separated groups of one to four lowercase hex digits, appended to
// `out` from `count`. False when a group is empty or too long, or the run holds too many.
[[nodiscard]] bool parse_ipv6_groups(std::string_view text, std::array<std::uint16_t, 8>& out,
                                     std::size_t& count) noexcept {
    if (text.empty()) {
        return true;
    }
    while (true) {
        const std::size_t colon = text.find(':');
        const std::string_view group = text.substr(0, colon);
        if (group.empty() || group.size() > 4 || count == out.size()) {
            return false;
        }
        unsigned value = 0;
        for (const char c : group) {
            if (c >= '0' && c <= '9') {
                value = (value * 16) + static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value = (value * 16) + static_cast<unsigned>(c - 'a' + 10);
            } else {
                return false;
            }
        }
        out.at(count++) = static_cast<std::uint16_t>(value);
        if (colon == std::string_view::npos) {
            return true;
        }
        text.remove_prefix(colon + 1);
    }
}

// True when `literal` (without its brackets) is an IPv6 address written exactly as the URL
// standard serialises it, which is how a browser writes the host in Origin: lowercase hex
// without leading zeros, the first longest run of two or more zero pieces as "::", and no dotted
// IPv4 tail. Origin is compared byte for byte, so any other spelling of the same address names
// a host no browser sends and is refused rather than kept as an entry that never matches.
[[nodiscard]] bool is_canonical_ipv6(std::string_view literal) noexcept {
    std::array<std::uint16_t, 8> pieces{};
    std::size_t count = 0;
    const std::size_t gap = literal.find("::");
    if (gap == std::string_view::npos) {
        if (!parse_ipv6_groups(literal, pieces, count) || count != pieces.size()) {
            return false;
        }
    } else {
        // The pieces after "::" go at the end; the zeros between are already in place.
        std::array<std::uint16_t, 8> tail{};
        std::size_t tail_count = 0;
        if (!parse_ipv6_groups(literal.substr(0, gap), pieces, count) ||
            !parse_ipv6_groups(literal.substr(gap + 2), tail, tail_count) ||
            count + tail_count >= pieces.size()) {
            return false;
        }
        std::ranges::copy(std::span{tail}.first(tail_count),
                          pieces.end() - static_cast<std::ptrdiff_t>(tail_count));
    }
    // The first longest run of at least two zero pieces is the one compressed.
    std::size_t run_start = pieces.size();
    std::size_t run_length = 1;
    for (std::size_t i = 0; i < pieces.size();) {
        std::size_t end = i;
        while (end < pieces.size() && pieces.at(end) == 0) {
            ++end;
        }
        if (end - i > run_length) {
            run_start = i;
            run_length = end - i;
        }
        i = end == i ? i + 1 : end;
    }
    // At most 8 groups of 4 digits and 7 separators.
    std::array<char, 40> buffer{};
    std::size_t length = 0;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        if (i == run_start) {
            buffer.at(length++) = ':';
            buffer.at(length++) = ':';
            i += run_length - 1;
            continue;
        }
        if (length != 0 && buffer.at(length - 1) != ':') {
            buffer.at(length++) = ':';
        }
        const auto written =
            std::to_chars(buffer.data() + length, buffer.data() + buffer.size(), pieces.at(i), 16);
        length = static_cast<std::size_t>(written.ptr - buffer.data());
    }
    return literal == std::string_view{buffer.data(), length};
}

// Whether `label` parses as an IPv4 number in the URL standard: decimal, octal after a leading
// 0, or hex after 0x, the prefix alone being zero.
[[nodiscard]] bool is_ipv4_number(std::string_view label) noexcept {
    if (label.empty()) {
        return false;
    }
    if (label.starts_with("0x")) {
        return std::ranges::all_of(label.substr(2), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }
    if (label.size() > 1 && label.front() == '0') {
        return std::ranges::all_of(label.substr(1), [](char c) { return c >= '0' && c <= '7'; });
    }
    return std::ranges::all_of(label, [](char c) { return c >= '0' && c <= '9'; });
}

// The URL standard's "ends in a number": the last label, past one trailing dot, is all digits or
// an IPv4 number. Such a host is an IPv4 address (or no URL at all), never a domain.
[[nodiscard]] bool ends_in_a_number(std::string_view host) noexcept {
    if (host.ends_with('.')) {
        if (host.find('.') == host.size() - 1) {
            return false;
        }
        host.remove_suffix(1);
    }
    const std::size_t dot = host.rfind('.');
    const std::string_view last = dot == std::string_view::npos ? host : host.substr(dot + 1);
    return (!last.empty() &&
            std::ranges::all_of(last, [](char c) { return c >= '0' && c <= '9'; })) ||
           is_ipv4_number(last);
}

// True when `host` is four decimal octets 0-255 without leading zeros, the only way a browser
// writes an IPv4 host.
[[nodiscard]] bool is_canonical_ipv4(std::string_view host) noexcept {
    for (int octet = 0; octet < 4; ++octet) {
        const std::size_t dot = host.find('.');
        if ((octet == 3) != (dot == std::string_view::npos)) {
            return false;
        }
        const std::string_view digits = host.substr(0, dot);
        if (digits.empty() || digits.size() > 3 || (digits.size() > 1 && digits.front() == '0')) {
            return false;
        }
        unsigned value = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = (value * 10) + static_cast<unsigned>(c - '0');
        }
        if (value > 255) {
            return false;
        }
        host = dot == std::string_view::npos ? std::string_view{} : host.substr(dot + 1);
    }
    return true;
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
        if (!is_canonical_ipv6(rest.substr(1, close - 1))) {
            return false;
        }
        host_end = close + 1;
    }
    host_end = std::min(host_end, rest.size());
    const std::string_view host = rest.substr(0, host_end);
    // A host ending in a number is an IPv4 address, which a browser writes only one way; any
    // other spelling (010.0.0.1, 0x7f.1, 127.1, 10.0.0.1.) or a number past 255 never matches.
    if (host.empty() || (plain && !is_loopback(host)) ||
        (!host.starts_with('[') && ends_in_a_number(host) && !is_canonical_ipv4(host))) {
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
