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

std::string IpAddress::to_string() const {
    std::array<char, INET6_ADDRSTRLEN> text{};
    const bool v4 = is_v4();
    const void* source = v4 ? static_cast<const void*>(&bytes_[kMappedPrefix.size()])
                            : static_cast<const void*>(bytes_.data());
    if (::inet_ntop(v4 ? AF_INET : AF_INET6, source, text.data(), text.size()) == nullptr) {
        return {};
    }
    return std::string(text.data());
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

std::optional<IpAddress> peer_address(int fd) noexcept {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return std::nullopt;
    }
    std::array<std::uint8_t, IpAddress::kBytes> bytes{};
    if (addr.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        std::memcpy(bytes.data(), &v6->sin6_addr, bytes.size());
        return IpAddress::from_bytes(bytes);
    }
    if (addr.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&addr);
        std::ranges::copy(kMappedPrefix, bytes.begin());
        std::memcpy(&bytes[kMappedPrefix.size()], &v4->sin_addr, sizeof v4->sin_addr);
        return IpAddress::from_bytes(bytes);
    }
    return std::nullopt;
}

} // namespace net
