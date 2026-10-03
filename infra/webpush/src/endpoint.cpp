#include "infra/webpush/endpoint.hpp"

#include "net/ip_address.hpp"

#include <algorithm>

namespace infra::webpush {

namespace {

constexpr std::string_view kScheme = "https://";
constexpr std::size_t kMaxLabel = 63;
constexpr std::size_t kMaxHost = 253;

char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), lower);
    return out;
}

bool is_ldh(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

// LDH labels joined by single dots: no empty label, no hyphen at either end of one, no trailing
// dot.
bool is_host_name(std::string_view host) noexcept {
    if (host.empty() || host.size() > kMaxHost) {
        return false;
    }
    while (true) {
        const std::size_t dot = host.find('.');
        const std::string_view label = host.substr(0, dot);
        if (label.empty() || label.size() > kMaxLabel || label.front() == '-' ||
            label.back() == '-' || !std::ranges::all_of(label, is_ldh)) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        host.remove_prefix(dot + 1);
    }
}

// A name whose last label is a number is an IPv4 address in one of the forms URL parsers
// normalise ("127.1", "0x7f.0.0.1", "2130706433"); no top-level domain is numeric.
bool looks_numeric(std::string_view host) noexcept {
    const std::size_t dot = host.rfind('.');
    const std::string_view last = dot == std::string_view::npos ? host : host.substr(dot + 1);
    const bool hex = last.size() > 2 && lower(last[0]) == '0' && lower(last[1]) == 'x';
    return hex || std::ranges::all_of(last, [](char c) { return c >= '0' && c <= '9'; });
}

// What may follow the host: RFC 3986's path and query characters, percent-encoding included,
// and nothing that ends the path in a fragment or that a URL never carries unencoded.
bool is_path_char(char c) noexcept {
    if (c <= ' ' || c >= '\x7f') {
        return false;
    }
    constexpr std::string_view kNever = "\"<>\\^`{|}#";
    return kNever.find(c) == std::string_view::npos;
}

} // namespace

std::expected<PushHosts, std::string> PushHosts::parse(std::string_view list) {
    PushHosts out;
    while (!list.empty() || out.entries_.empty()) {
        const std::size_t comma = list.find(',');
        std::string_view entry = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) {
            entry.remove_prefix(1);
        }
        while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) {
            entry.remove_suffix(1);
        }
        const bool wildcard = entry.starts_with("*.");
        const std::string_view name = wildcard ? entry.substr(2) : entry;
        if (!is_host_name(name) || looks_numeric(name)) {
            return std::unexpected("expected comma-separated host names, or *.domain");
        }
        // A wildcard over a bare top-level domain ("*.com") would allow nearly anything.
        if (wildcard && name.find('.') == std::string_view::npos) {
            return std::unexpected("a wildcard needs a domain of two labels or more");
        }
        if (out.entries_.size() == kMaxPushHosts) {
            return std::unexpected("more than 64 hosts");
        }
        out.entries_.push_back(lowered(entry));
        if (comma == std::string_view::npos) {
            break;
        }
    }
    return out;
}

PushHosts PushHosts::defaults() {
    // The constant is a valid list; parse cannot refuse it.
    return parse(kDefaultPushHosts).value_or(PushHosts{});
}

bool PushHosts::allows(std::string_view host) const noexcept {
    return std::ranges::any_of(entries_, [&](const std::string& entry) {
        if (!entry.starts_with("*.")) {
            return host.size() == entry.size() &&
                   std::ranges::equal(host, entry, [](char a, char b) { return lower(a) == b; });
        }
        // "*.example.com" matches "a.example.com", never "example.com" or "aexample.com".
        const std::string_view suffix = std::string_view(entry).substr(1);
        return host.size() > suffix.size() + 1 &&
               std::ranges::equal(host.substr(host.size() - suffix.size()), suffix,
                                  [](char a, char b) { return lower(a) == b; });
    });
}

std::expected<Endpoint, EndpointError> check_endpoint(std::string_view url, const PushHosts& hosts,
                                                      bool any_port) {
    if (url.size() > kMaxEndpointBytes) {
        return std::unexpected(EndpointError::TooLong);
    }
    if (url.size() < kScheme.size() ||
        !std::ranges::equal(url.substr(0, kScheme.size()), kScheme,
                            [](char a, char b) { return lower(a) == b; })) {
        return std::unexpected(EndpointError::NotHttps);
    }
    const std::string_view rest = url.substr(kScheme.size());
    const std::size_t path = rest.find('/');
    if (path == std::string_view::npos || !std::ranges::all_of(rest, is_path_char)) {
        return std::unexpected(EndpointError::Malformed);
    }
    std::string_view authority = rest.substr(0, path);
    if (authority.find('@') != std::string_view::npos) {
        return std::unexpected(EndpointError::Malformed);
    }
    if (authority.starts_with('[')) {
        return std::unexpected(EndpointError::AddressLiteral);
    }
    std::string_view port;
    if (const std::size_t colon = authority.find(':'); colon != std::string_view::npos) {
        port = authority.substr(colon + 1);
        const bool other = any_port && !port.empty() && port.size() <= 5 && port.front() != '0' &&
                           std::ranges::all_of(port, [](char c) { return c >= '0' && c <= '9'; });
        if (port != "443" && !other) {
            return std::unexpected(EndpointError::NotHttps);
        }
        authority = authority.substr(0, colon);
    }
    if (net::IpAddress::parse(authority).has_value() ||
        (is_host_name(authority) && looks_numeric(authority))) {
        return std::unexpected(EndpointError::AddressLiteral);
    }
    if (!is_host_name(authority)) {
        return std::unexpected(EndpointError::Malformed);
    }
    if (!hosts.allows(authority)) {
        return std::unexpected(EndpointError::HostNotAllowed);
    }
    // The origin leaves out the default port (RFC 6454 section 6.2).
    std::string origin = "https://" + lowered(authority);
    if (!port.empty() && port != "443") {
        origin += ':';
        origin += port;
    }
    return Endpoint{.url = std::string(url), .origin = std::move(origin)};
}

std::string_view to_string(EndpointError error) noexcept {
    switch (error) {
    case EndpointError::TooLong:
        return "too_long";
    case EndpointError::NotHttps:
        return "not_https";
    case EndpointError::Malformed:
        return "malformed";
    case EndpointError::AddressLiteral:
        return "address_literal";
    case EndpointError::HostNotAllowed:
        return "host_not_allowed";
    }
    return "malformed";
}

} // namespace infra::webpush
