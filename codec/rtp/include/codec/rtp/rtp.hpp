#pragma once

#include "codec/rtp/entries.hpp"
#include "codec/rtp/error.hpp"
#include "core/util/lifetime.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

// RTP (RFC 3550 section 5) and its header extensions (RFC 8285), read in place: every span
// points into the datagram given to parse_rtp(), which must outlive the packet.
namespace codec::rtp {

// RFC 8285 section 4.2: "defined by profile" 0xBEDE announces one-byte element headers.
inline constexpr std::uint16_t kOneByteProfile = 0xBEDE;
// RFC 8285 section 4.3: 0x100 in the top 12 bits announces two-byte element headers; the low
// four bits are application data.
inline constexpr std::uint16_t kTwoByteProfile = 0x1000;
inline constexpr std::uint16_t kTwoByteProfileMask = 0xFFF0;

struct HeaderExtension {
    std::uint16_t profile = 0;
    // The extension's length field words, without its 4-byte header.
    std::span<const std::byte> data;
};

struct ExtensionElement {
    std::uint8_t id = 0;
    std::span<const std::byte> data;
};

// The elements of an RFC 8285 extension, skipping padding. An extension under any other
// profile is that profile's business (RFC 3550 section 5.3.1) and yields no elements.
class ExtensionElements {
public:
    explicit ExtensionElements(const HeaderExtension& extension) noexcept;

    [[nodiscard]] std::optional<ExtensionElement> next() noexcept;
    // An element ran past the end of the extension. parse_rtp() refuses such packets, so this
    // is only ever true for an extension built by hand.
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }

private:
    enum class Form : std::uint8_t { None, OneByte, TwoByte };

    Form form_ = Form::None;
    std::span<const std::byte> rest_;
    bool malformed_ = false;
};

struct RtpPacket {
    bool marker = false;
    std::uint8_t payload_type = 0;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    Entries<std::uint32_t> csrcs;
    std::optional<HeaderExtension> extension;
    std::span<const std::byte> payload;
    // Bytes of padding after the payload, the count byte included; 0 without the P bit.
    std::uint8_t padding = 0;
};

[[nodiscard]] std::expected<RtpPacket, Error>
parse_rtp(std::span<const std::byte> datagram ULW_LIFETIMEBOUND) noexcept;
// The packet points into the datagram, which a temporary buffer would take with it.
std::expected<RtpPacket, Error> parse_rtp(std::vector<std::byte>&&) noexcept = delete;

} // namespace codec::rtp
