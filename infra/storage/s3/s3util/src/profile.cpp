#include "infra/s3util/profile.hpp"

#include "decimal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace infra::s3util {

namespace {

constexpr std::uint16_t kHttpPort = 80;
constexpr std::uint16_t kHttpsPort = 443;

bool is_lower_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool is_lower_hex(char c) noexcept {
    return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
}

// RFC 1123 label, lowercase only so every string spliced into a hostname is already in the
// form the Host header will carry.
bool is_dns_label(std::string_view s) noexcept {
    // 63: RFC 1035 label limit.
    return !s.empty() && s.size() <= 63 && is_lower_alnum(s.front()) && is_lower_alnum(s.back()) &&
           std::ranges::all_of(s, [](char c) { return is_lower_alnum(c) || c == '-'; });
}

bool is_hostname(std::string_view s) noexcept {
    // 253: RFC 1035 name limit without the trailing root dot.
    if (s.empty() || s.size() > 253) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = s.find('.', start);
        if (!is_dns_label(s.substr(start, dot - start))) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        start = dot + 1;
    }
}

bool is_region(std::string_view s) noexcept {
    return is_dns_label(s) && s.front() >= 'a' && s.front() <= 'z';
}

bool is_r2_account_id(std::string_view s) noexcept {
    // Cloudflare account ids are 128-bit, printed as 32 lowercase hex digits.
    return s.size() == 32 && std::ranges::all_of(s, is_lower_hex);
}

S3Profile https_profile(std::string host, std::string region, Addressing addressing) {
    S3Profile p;
    p.endpoint = Endpoint{.scheme = Scheme::Https, .host = std::move(host), .port = kHttpsPort};
    p.region = std::move(region);
    p.addressing = addressing;
    return p;
}

} // namespace

std::expected<Endpoint, ProfileError> parse_endpoint(std::string_view url) {
    Endpoint e;
    if (url.starts_with("https://")) {
        e.scheme = Scheme::Https;
        e.port = kHttpsPort;
        url.remove_prefix(std::string_view("https://").size());
    } else if (url.starts_with("http://")) {
        e.scheme = Scheme::Http;
        e.port = kHttpPort;
        url.remove_prefix(std::string_view("http://").size());
    } else {
        return std::unexpected(ProfileError::InvalidEndpoint);
    }
    const std::size_t colon = url.find(':');
    const std::string_view host = url.substr(0, colon);
    if (!is_hostname(host)) {
        return std::unexpected(ProfileError::InvalidEndpoint);
    }
    if (colon != std::string_view::npos) {
        const auto port = detail::parse_decimal<std::uint16_t>(url.substr(colon + 1));
        if (!port || *port == 0) {
            return std::unexpected(ProfileError::InvalidEndpoint);
        }
        e.port = *port;
    }
    e.host = std::string(host);
    return e;
}

std::string authority(const Endpoint& endpoint) {
    const std::uint16_t default_port = endpoint.scheme == Scheme::Https ? kHttpsPort : kHttpPort;
    if (endpoint.port == default_port) {
        return endpoint.host;
    }
    return endpoint.host + ':' + std::to_string(endpoint.port);
}

// MinIO signs with its configured region, which is us-east-1 unless an operator changed it.
// Current releases honour If-None-Match on PUT.
std::expected<S3Profile, ProfileError> S3Profile::minio(std::string_view endpoint) {
    auto parsed = parse_endpoint(endpoint);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    S3Profile p;
    p.endpoint = std::move(*parsed);
    p.region = "us-east-1";
    p.addressing = Addressing::PathStyle;
    p.supports_conditional_put = true;
    return p;
}

std::expected<S3Profile, ProfileError> S3Profile::aws(std::string_view region) {
    if (!is_region(region)) {
        return std::unexpected(ProfileError::InvalidRegion);
    }
    auto p = https_profile("s3." + std::string(region) + ".amazonaws.com", std::string(region),
                           Addressing::VirtualHosted);
    p.supports_conditional_put = true;
    return p;
}

// R2 has one logical region, "auto", and rejects a completion whose non-final parts differ
// in size, so the uploader must cut every part but the last to the same length.
std::expected<S3Profile, ProfileError> S3Profile::r2(std::string_view account_id) {
    if (!is_r2_account_id(account_id)) {
        return std::unexpected(ProfileError::InvalidAccountId);
    }
    auto p = https_profile(std::string(account_id) + ".r2.cloudflarestorage.com", "auto",
                           Addressing::PathStyle);
    p.uniform_parts_required = true;
    p.supports_conditional_put = true;
    return p;
}

// B2 states its part limits in decimal units, 5 MB to 5 GB: the 5 MiB default already clears
// the floor, but the ceiling has to come down from 5 GiB. Its S3 API has no conditional PUT.
std::expected<S3Profile, ProfileError> S3Profile::b2(std::string_view region) {
    if (!is_region(region)) {
        return std::unexpected(ProfileError::InvalidRegion);
    }
    auto p = https_profile("s3." + std::string(region) + ".backblazeb2.com", std::string(region),
                           Addressing::PathStyle);
    p.max_part_bytes = 5'000'000'000;
    return p;
}

} // namespace infra::s3util
