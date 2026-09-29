#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace net {

// An IPv4 or IPv6 address, IPv4 held as its IPv4-mapped IPv6 form (::ffff:a.b.c.d), so a
// dual-stack listener's peers and a proxy's X-Forwarded-For entries compare alike.
class IpAddress {
public:
    static constexpr std::size_t kBytes = 16;

    // ::, which no peer ever has.
    IpAddress() noexcept = default;

    // Numeric only: dotted IPv4, or IPv6 without brackets, port or zone.
    [[nodiscard]] static std::optional<IpAddress> parse(std::string_view text) noexcept;
    [[nodiscard]] static IpAddress
    from_bytes(const std::array<std::uint8_t, kBytes>& bytes) noexcept {
        return IpAddress(bytes);
    }

    [[nodiscard]] const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool is_v4() const noexcept;
    // INET6_ADDRSTRLEN: the longest text form, "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"
    // and its terminator.
    static constexpr std::size_t kTextLength = 46;
    using Text = std::array<char, kTextLength>;
    // Dotted for IPv4, RFC 5952 text for IPv6, written into `out` without allocating; for logs.
    [[nodiscard]] std::string_view format(Text& out) const noexcept;
    // The same address with every bit past the first `bits` cleared.
    [[nodiscard]] IpAddress prefix(unsigned bits) const noexcept;

    friend bool operator==(const IpAddress&, const IpAddress&) noexcept = default;

private:
    explicit IpAddress(const std::array<std::uint8_t, kBytes>& bytes) noexcept : bytes_(bytes) {}

    std::array<std::uint8_t, kBytes> bytes_{};
};

// An address block in CIDR notation: "10.42.0.0/16", "fd00::/8". A bare address is its own
// /32 or /128.
class IpNetwork {
public:
    // Refuses host bits set past the prefix: "10.42.1.0/16" is more likely a typo than a
    // wish for 10.42.0.0/16, and a block that means something other than it says would trust
    // the wrong peers.
    [[nodiscard]] static std::optional<IpNetwork> parse(std::string_view text) noexcept;

    [[nodiscard]] bool contains(const IpAddress& address) const noexcept;
    [[nodiscard]] bool is_v4() const noexcept { return base_.is_v4(); }
    // The prefix length as written: an IPv4 /16 is 16.
    [[nodiscard]] unsigned prefix_length() const noexcept {
        return base_.is_v4() ? bits_ - kV4MappedBits : bits_;
    }

    friend bool operator==(const IpNetwork&, const IpNetwork&) noexcept = default;

private:
    static constexpr unsigned kV4MappedBits = 96;

    IpNetwork(IpAddress base, unsigned bits) noexcept : base_(base), bits_(bits) {}

    IpAddress base_;
    // Over the 128-bit form: an IPv4 /16 is stored as /112.
    unsigned bits_ = 0;
};

// The address at the other end of a connected socket; nullopt once the peer has gone.
[[nodiscard]] std::optional<IpAddress> peer_address(int fd) noexcept;

} // namespace net
