#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace codec::rtp {

enum class PacketKind : std::uint8_t { Rtp, Rtcp, Other };

// RTP and RTCP on one port (RFC 5761 section 4): the second byte of an RTCP packet is its type,
// 192 to 223 for every type in use, which RTP payload types 64 to 95 with the marker bit set
// would collide with; RFC 5761 therefore keeps those payload types out of multiplexed
// sessions. Anything that is not version 2 (STUN, DTLS, as RFC 7983 sorts them) is Other.
[[nodiscard]] constexpr PacketKind classify(std::span<const std::byte> datagram) noexcept {
    constexpr std::size_t kMinimum = 2;
    if (datagram.size() < kMinimum) {
        return PacketKind::Other;
    }
    const auto first = std::to_integer<std::uint8_t>(datagram[0]);
    const auto second = std::to_integer<std::uint8_t>(datagram[1]);
    constexpr std::uint8_t kVersion2 = 0x80;
    constexpr std::uint8_t kVersionBits = 0xC0;
    if ((first & kVersionBits) != kVersion2) {
        return PacketKind::Other;
    }
    constexpr std::uint8_t kFirstRtcpType = 192;
    constexpr std::uint8_t kLastRtcpType = 223;
    return second >= kFirstRtcpType && second <= kLastRtcpType ? PacketKind::Rtcp : PacketKind::Rtp;
}

} // namespace codec::rtp
