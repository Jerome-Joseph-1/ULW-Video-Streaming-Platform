#include "http/client_limits.hpp"

#include <algorithm>
#include <cstring>
#include <ranges>

namespace http {

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return lower(x) == lower(y);
    });
}

std::string_view trim(std::string_view s) noexcept {
    const std::size_t first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

} // namespace

net::IpAddress forwarded_client(const net::IpAddress& peer, std::span<const HeaderField> headers,
                                std::size_t hops) noexcept {
    std::size_t seen = 0;
    // Repeated fields are one list in order (RFC 9110 section 5.3), so the count runs from the
    // last entry of the last field. Envoy merges a client's repeated X-Forwarded-For fields into
    // one before appending its own entry, so behind it there is one field; a peer that sends
    // several is read the same way.
    for (const HeaderField& field : std::views::reverse(headers)) {
        if (!iequals(field.name, "x-forwarded-for")) {
            continue;
        }
        std::string_view rest = field.value;
        while (!rest.empty()) {
            const std::size_t comma = rest.rfind(',');
            const std::string_view entry =
                trim(comma == std::string_view::npos ? rest : rest.substr(comma + 1));
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(0, comma);
            if (++seen == hops) {
                return net::IpAddress::parse(entry).value_or(peer);
            }
        }
    }
    return peer;
}

std::uint64_t SeededHash::operator()(std::span<const std::byte> bytes) const noexcept {
    // Eight bytes at a time through a multiply and a fold (the constant is 2^64 / phi, whose
    // bits are spread evenly), then a final avalanche so the low bits the table indexes by
    // depend on every input bit.
    constexpr std::uint64_t kMul = 0x9e3779b97f4a7c15ULL;
    std::uint64_t h = seed_ ^ (bytes.size() * kMul);
    while (!bytes.empty()) {
        std::uint64_t word = 0;
        const std::size_t n = std::min(bytes.size(), sizeof word);
        std::memcpy(&word, bytes.data(), n);
        bytes = bytes.subspan(n);
        h = (h ^ word) * kMul;
        h ^= h >> 32U;
    }
    h ^= h >> 29U;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 32U;
    return h;
}

std::expected<std::vector<net::IpNetwork>, std::string>
parse_trusted_proxies(std::string_view text) {
    std::vector<net::IpNetwork> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = trim(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        const auto network = net::IpNetwork::parse(item);
        if (!network) {
            return std::unexpected("expected comma-separated CIDR blocks, such as 10.42.0.0/16, "
                                   "with no bits set past the prefix");
        }
        // Every address on the internet could then name any client it liked.
        if (network->prefix_length() == 0) {
            return std::unexpected("a /0 block trusts every peer");
        }
        out.push_back(*network);
    }
    if (out.size() > kMaxTrustedProxies) {
        return std::unexpected("more than 16 blocks");
    }
    return out;
}

bool is_trusted_proxy(std::span<const net::IpNetwork> proxies,
                      const net::IpAddress& peer) noexcept {
    return std::ranges::any_of(proxies, [&](const net::IpNetwork& n) { return n.contains(peer); });
}

} // namespace http
