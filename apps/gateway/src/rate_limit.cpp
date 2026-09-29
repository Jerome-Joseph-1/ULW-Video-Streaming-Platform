#include "rate_limit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ranges>

namespace gateway {

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

std::expected<void, core::Millis> TokenBucket::take(const BucketRule& rule, core::MonoTime now,
                                                    double n) noexcept {
    if (now > last_refill_) {
        const double elapsed = std::chrono::duration<double>(now - last_refill_).count();
        tokens_ = std::min(rule.burst, tokens_ + (elapsed * rule.per_second));
        last_refill_ = now;
    }
    if (tokens_ >= n) {
        tokens_ -= n;
        return {};
    }
    const double seconds = (n - tokens_) / rule.per_second;
    return std::unexpected(core::Millis{static_cast<core::Millis::rep>(std::ceil(seconds * 1000))});
}

std::chrono::seconds retry_after(core::Millis wait) noexcept {
    const auto whole = std::chrono::ceil<std::chrono::seconds>(wait);
    return std::max(whole, std::chrono::seconds{1});
}

net::IpAddress forwarded_client(const net::IpAddress& peer,
                                std::span<const http::HeaderField> headers,
                                std::span<const net::IpNetwork> trusted) noexcept {
    const auto is_trusted = [&](const net::IpAddress& a) {
        return std::ranges::any_of(trusted, [&](const net::IpNetwork& n) { return n.contains(a); });
    };
    net::IpAddress client = peer;
    // Repeated fields are one list in order (RFC 9110 section 5.3), so the walk runs from the
    // last entry of the last field.
    for (const http::HeaderField& field : std::views::reverse(headers)) {
        if (!iequals(field.name, "x-forwarded-for")) {
            continue;
        }
        std::string_view rest = field.value;
        while (!rest.empty()) {
            const std::size_t comma = rest.rfind(',');
            const std::string_view entry =
                trim(comma == std::string_view::npos ? rest : rest.substr(comma + 1));
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(0, comma);
            const auto address = net::IpAddress::parse(entry);
            if (!address) {
                return client;
            }
            client = *address;
            if (!is_trusted(client)) {
                return client;
            }
        }
    }
    return client;
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

} // namespace gateway
