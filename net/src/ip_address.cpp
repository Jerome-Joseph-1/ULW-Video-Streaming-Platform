#include "net/ip_address.hpp"

#include "core/util/parse.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstring>

namespace net {

namespace {

// ::ffff:0:0/96, the prefix an IPv4-mapped address carries.
constexpr std::array<std::uint8_t, 12> kMappedPrefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

} // namespace

std::optional<IpAddress> IpAddress::parse(std::string_view text) noexcept {
    // inet_pton wants a terminated string; INET6_ADDRSTRLEN bounds any numeric address.
    std::array<char, INET6_ADDRSTRLEN> buf{};
    if (text.empty() || text.size() >= buf.size()) {
        return std::nullopt;
    }
    text.copy(buf.data(), text.size());
    std::array<std::uint8_t, kBytes> bytes{};
    if (::inet_pton(AF_INET6, buf.data(), bytes.data()) == 1) {
        return IpAddress(bytes);
    }
    in_addr v4{};
    if (::inet_pton(AF_INET, buf.data(), &v4) != 1) {
        return std::nullopt;
    }
    std::ranges::copy(kMappedPrefix, bytes.begin());
    std::memcpy(&bytes[kMappedPrefix.size()], &v4, sizeof v4);
    return IpAddress(bytes);
}

bool IpAddress::is_v4() const noexcept {
    return std::equal(kMappedPrefix.begin(), kMappedPrefix.end(), bytes_.begin());
}

std::string_view IpAddress::format(Text& out) const noexcept {
    static_assert(kTextLength == INET6_ADDRSTRLEN);
    const bool v4 = is_v4();
    const void* source = v4 ? static_cast<const void*>(&bytes_[kMappedPrefix.size()])
                            : static_cast<const void*>(bytes_.data());
    if (::inet_ntop(v4 ? AF_INET : AF_INET6, source, out.data(),
                    static_cast<socklen_t>(out.size())) == nullptr) {
        return {};
    }
    return {out.data()};
}

IpAddress IpAddress::prefix(unsigned bits) const noexcept {
    std::array<std::uint8_t, kBytes> out{};
    for (std::size_t i = 0; i < kBytes; ++i) {
        const unsigned start = static_cast<unsigned>(i) * 8U;
        if (bits >= start + 8U) {
            out.at(i) = bytes_.at(i);
        } else if (bits > start) {
            const unsigned keep = bits - start;
            out.at(i) = static_cast<std::uint8_t>(bytes_.at(i) & (0xffU << (8U - keep)));
        }
    }
    return IpAddress(out);
}

std::optional<IpNetwork> IpNetwork::parse(std::string_view text) noexcept {
    const std::size_t slash = text.find('/');
    const auto base = IpAddress::parse(text.substr(0, slash));
    if (!base) {
        return std::nullopt;
    }
    const unsigned width = base->is_v4() ? 32 : 128;
    unsigned bits = width;
    if (slash != std::string_view::npos) {
        const auto parsed = core::parse_integer<unsigned>(text.substr(slash + 1));
        if (!parsed || *parsed > width) {
            return std::nullopt;
        }
        bits = *parsed;
    }
    const unsigned stored = base->is_v4() ? bits + kV4MappedBits : bits;
    if (base->prefix(stored) != *base) {
        return std::nullopt;
    }
    return IpNetwork(*base, stored);
}

bool IpNetwork::contains(const IpAddress& address) const noexcept {
    return address.prefix(bits_) == base_;
}

std::optional<IpAddress> address_of(const sockaddr* address, std::size_t length) noexcept {
    if (address == nullptr) {
        return std::nullopt;
    }
    std::array<std::uint8_t, IpAddress::kBytes> bytes{};
    if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        std::memcpy(bytes.data(), &v6->sin6_addr, bytes.size());
        return IpAddress::from_bytes(bytes);
    }
    if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        std::ranges::copy(kMappedPrefix, bytes.begin());
        std::memcpy(&bytes[kMappedPrefix.size()], &v4->sin_addr, sizeof v4->sin_addr);
        return IpAddress::from_bytes(bytes);
    }
    return std::nullopt;
}

std::optional<IpAddress> peer_address(int fd) noexcept {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return std::nullopt;
    }
    return address_of(reinterpret_cast<const sockaddr*>(&addr), len);
}

namespace {

// The blocks no connection to the public internet goes to. IPv4 is held IPv4-mapped, so its
// blocks are /96 more than written. RFC 6890's special-purpose registries, plus multicast and
// the transition prefixes.
struct Block {
    std::array<std::uint8_t, IpAddress::kBytes> base;
    unsigned bits;
};

constexpr std::array<std::uint8_t, IpAddress::kBytes> v4(std::uint8_t a, std::uint8_t b,
                                                         std::uint8_t c, std::uint8_t d) {
    return {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, a, b, c, d};
}

constexpr std::array<std::uint8_t, IpAddress::kBytes> v6(std::uint8_t a, std::uint8_t b,
                                                         std::uint8_t c = 0, std::uint8_t d = 0) {
    return {a, b, c, d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}

constexpr unsigned kMapped = 96;

constexpr std::array kRefusedV4{
    Block{v4(0, 0, 0, 0), kMapped + 8},       // "this network"
    Block{v4(10, 0, 0, 0), kMapped + 8},      // RFC 1918
    Block{v4(100, 64, 0, 0), kMapped + 10},   // shared address space (CGNAT)
    Block{v4(127, 0, 0, 0), kMapped + 8},     // loopback
    Block{v4(169, 254, 0, 0), kMapped + 16},  // link-local, cloud metadata
    Block{v4(172, 16, 0, 0), kMapped + 12},   // RFC 1918
    Block{v4(192, 0, 0, 0), kMapped + 24},    // IETF protocol assignments
    Block{v4(192, 0, 2, 0), kMapped + 24},    // TEST-NET-1
    Block{v4(192, 88, 99, 0), kMapped + 24},  // 6to4 relay anycast
    Block{v4(192, 168, 0, 0), kMapped + 16},  // RFC 1918
    Block{v4(198, 18, 0, 0), kMapped + 15},   // benchmarking
    Block{v4(198, 51, 100, 0), kMapped + 24}, // TEST-NET-2
    Block{v4(203, 0, 113, 0), kMapped + 24},  // TEST-NET-3
    Block{v4(224, 0, 0, 0), kMapped + 4},     // multicast
    Block{v4(240, 0, 0, 0), kMapped + 4},     // reserved, and the broadcast address
};

// Inside 2000::/3, the global unicast block every other IPv6 address is refused outside of.
constexpr std::array kRefusedV6{
    Block{v6(0x20, 0x01, 0x00, 0x00), 23}, // IETF protocol assignments, Teredo
    Block{v6(0x20, 0x01, 0x0d, 0xb8), 32}, // documentation
    Block{v6(0x20, 0x02), 16},             // 6to4
    Block{v6(0x3f, 0xff, 0x00, 0x00), 20}, // documentation (RFC 9637)
};

bool within(const IpAddress& address, const Block& block) noexcept {
    return address.prefix(block.bits).bytes() == block.base;
}

} // namespace

bool is_global_unicast(const IpAddress& address) noexcept {
    if (address.is_v4()) {
        return std::ranges::none_of(kRefusedV4, [&](const Block& b) { return within(address, b); });
    }
    // 2000::/3: the first three bits are 001.
    constexpr std::uint8_t kTopThree = 0xe0;
    constexpr std::uint8_t kGlobal = 0x20;
    if ((address.bytes()[0] & kTopThree) != kGlobal) {
        return false;
    }
    return std::ranges::none_of(kRefusedV6, [&](const Block& b) { return within(address, b); });
}

} // namespace net
