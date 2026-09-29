#include "codec/rtp/rtp.hpp"

#include "codec/rtp/entries.hpp"
#include "codec/rtp/error.hpp"

#include "bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace codec::rtp {
namespace {

using detail::be16;
using detail::be32;
using detail::u8;

// RFC 3550 section 5.1.
constexpr std::size_t kFixedHeader = 12;
constexpr std::size_t kWord = 4;
constexpr std::uint8_t kVersionBits = 0xC0;
constexpr std::uint8_t kVersion2 = 0x80;
constexpr std::uint8_t kPaddingBit = 0x20;
constexpr std::uint8_t kExtensionBit = 0x10;
constexpr std::uint8_t kCsrcCountBits = 0x0F;
constexpr std::uint8_t kMarkerBit = 0x80;
constexpr std::uint8_t kPayloadTypeBits = 0x7F;

} // namespace

std::uint32_t WireFormat<std::uint32_t>::read(std::span<const std::byte, kSize> bytes) noexcept {
    return be32(bytes, 0);
}

ExtensionElements::ExtensionElements(const HeaderExtension& extension) noexcept
    : rest_(extension.data) {
    if (extension.profile == kOneByteProfile) {
        form_ = Form::OneByte;
    } else if ((extension.profile & kTwoByteProfileMask) == kTwoByteProfile) {
        form_ = Form::TwoByte;
    }
}

std::optional<ExtensionElement> ExtensionElements::next() noexcept {
    // RFC 8285 section 4.2: in the one-byte form, ID 0 is a padding byte whose length nibble is
    // ignored, and ID 15 ends processing of the whole extension.
    constexpr std::uint8_t kPadding = 0;
    constexpr std::uint8_t kOneByteStop = 15;
    while (!rest_.empty() && form_ != Form::None) {
        const std::uint8_t first = u8(rest_, 0);
        if (form_ == Form::OneByte) {
            const auto id = static_cast<std::uint8_t>(first >> 4U);
            if (id == kPadding) {
                rest_ = rest_.subspan(1);
                continue;
            }
            if (id == kOneByteStop) {
                rest_ = {};
                break;
            }
            // The nibble is the length minus one: 1 to 16 bytes.
            const std::size_t length = (first & 0x0FU) + 1U;
            if (rest_.size() < 1 + length) {
                malformed_ = true;
                break;
            }
            const ExtensionElement e{.id = id, .data = rest_.subspan(1, length)};
            rest_ = rest_.subspan(1 + length);
            return e;
        }
        // RFC 8285 section 4.3: a zero byte is padding; otherwise ID, length (0 to 255), data.
        if (first == kPadding) {
            rest_ = rest_.subspan(1);
            continue;
        }
        if (rest_.size() < 2 || rest_.size() - 2 < u8(rest_, 1)) {
            malformed_ = true;
            break;
        }
        const std::size_t length = u8(rest_, 1);
        const ExtensionElement e{.id = first, .data = rest_.subspan(2, length)};
        rest_ = rest_.subspan(2 + length);
        return e;
    }
    rest_ = {};
    return std::nullopt;
}

std::expected<RtpPacket, Error> parse_rtp(std::span<const std::byte> datagram) noexcept {
    if (datagram.size() < kFixedHeader) {
        return std::unexpected{Error::Truncated};
    }
    const std::uint8_t b0 = u8(datagram, 0);
    const std::uint8_t b1 = u8(datagram, 1);
    if ((b0 & kVersionBits) != kVersion2) {
        return std::unexpected{Error::BadVersion};
    }
    RtpPacket p;
    p.marker = (b1 & kMarkerBit) != 0;
    p.payload_type = b1 & kPayloadTypeBits;
    p.sequence = be16(datagram, 2);
    p.timestamp = be32(datagram, 4);
    p.ssrc = be32(datagram, 8);

    std::size_t header = kFixedHeader;
    const std::size_t csrc_bytes = kWord * (b0 & kCsrcCountBits);
    if (datagram.size() - header < csrc_bytes) {
        return std::unexpected{Error::Truncated};
    }
    p.csrcs = Entries<std::uint32_t>{datagram.subspan(header, csrc_bytes)};
    header += csrc_bytes;

    if ((b0 & kExtensionBit) != 0) {
        // RFC 3550 section 5.3.1: 16 bits of profile, 16 bits of length in words, the words.
        if (datagram.size() - header < kWord) {
            return std::unexpected{Error::Truncated};
        }
        const std::uint16_t profile = be16(datagram, header);
        const std::size_t length = kWord * be16(datagram, header + 2);
        header += kWord;
        if (datagram.size() - header < length) {
            return std::unexpected{Error::Truncated};
        }
        p.extension = HeaderExtension{.profile = profile, .data = datagram.subspan(header, length)};
        header += length;
        ExtensionElements elements{*p.extension};
        while (elements.next()) {
        }
        if (elements.malformed()) {
            return std::unexpected{Error::BadExtension};
        }
    }

    std::size_t end = datagram.size();
    if ((b0 & kPaddingBit) != 0) {
        // RFC 3550 section 5.1: the last byte counts the padding, itself included.
        const std::uint8_t padding = u8(datagram, end - 1);
        if (padding == 0 || padding > end - header) {
            return std::unexpected{Error::BadPadding};
        }
        p.padding = padding;
        end -= padding;
    }
    p.payload = datagram.subspan(header, end - header);
    return p;
}

} // namespace codec::rtp
