#include "codec/rtp/rtcp.hpp"

#include "codec/rtp/entries.hpp"
#include "codec/rtp/error.hpp"

#include "bytes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace codec::rtp {
namespace {

using detail::be16;
using detail::be24;
using detail::be32;
using detail::be64;
using detail::signed24;
using detail::u8;

// RFC 3550 section 6.4.1.
constexpr std::size_t kHeader = 4;
constexpr std::size_t kWord = 4;
constexpr std::uint8_t kVersionBits = 0xC0;
constexpr std::uint8_t kVersion2 = 0x80;
constexpr std::uint8_t kPaddingBit = 0x20;
constexpr std::uint8_t kCountBits = 0x1F;

// SSRC, NTP timestamp (8), RTP timestamp, packet and octet counts.
constexpr std::size_t kSenderInfo = 24;
// Packet sender and media source SSRCs (RFC 4585 section 6.1).
constexpr std::size_t kFeedbackHeader = 8;

// RFC 4585 section 6.2 and 6.3, RFC 5104 section 4.3, and the transport-wide-cc and REMB
// drafts: the formats in the count field.
constexpr std::uint8_t kGenericNack = 1;
constexpr std::uint8_t kTransportCc = 15;
constexpr std::uint8_t kPli = 1;
constexpr std::uint8_t kFir = 4;
constexpr std::uint8_t kApplicationLayer = 15;

std::expected<RtcpPacket, Error> reports(bool sender, std::uint8_t count,
                                         std::span<const std::byte> body) noexcept {
    const std::size_t fixed = sender ? kSenderInfo : kWord;
    if (body.size() < fixed) {
        return std::unexpected{Error::Truncated};
    }
    const std::size_t blocks = WireFormat<ReportBlock>::kSize * count;
    if (body.size() - fixed < blocks) {
        return std::unexpected{Error::BadReportCount};
    }
    const Entries<ReportBlock> entries{body.subspan(fixed, blocks)};
    const std::span<const std::byte> extension = body.subspan(fixed + blocks);
    if (!sender) {
        return ReceiverReport{.ssrc = be32(body, 0), .reports = entries, .extension = extension};
    }
    return SenderReport{.ssrc = be32(body, 0),
                        .ntp_timestamp = be64(body, 4),
                        .rtp_timestamp = be32(body, 12),
                        .packet_count = be32(body, 16),
                        .octet_count = be32(body, 20),
                        .reports = entries,
                        .extension = extension};
}

std::expected<RtcpPacket, Error> source_description(std::uint8_t count,
                                                    std::span<const std::byte> body) noexcept {
    SdesItems items{body, count};
    while (items.next()) {
    }
    if (items.malformed()) {
        return std::unexpected{Error::BadSourceDescription};
    }
    return SourceDescription{.chunk_count = count, .chunks = body};
}

std::expected<RtcpPacket, Error> bye(std::uint8_t count, std::span<const std::byte> body) noexcept {
    const std::size_t ssrcs = kWord * count;
    if (body.size() < ssrcs) {
        return std::unexpected{Error::BadBye};
    }
    Bye b{.ssrcs = Entries<std::uint32_t>{body.first(ssrcs)}, .reason = {}};
    // RFC 3550 section 6.6: an optional length-prefixed reason, padded to a word.
    const std::span<const std::byte> rest = body.subspan(ssrcs);
    if (!rest.empty()) {
        const std::size_t length = u8(rest, 0);
        if (rest.size() - 1 < length) {
            return std::unexpected{Error::BadBye};
        }
        b.reason = rest.subspan(1, length);
    }
    return b;
}

std::expected<RtcpPacket, Error> app(std::uint8_t subtype,
                                     std::span<const std::byte> body) noexcept {
    constexpr std::size_t kSsrcAndName = 8;
    if (body.size() < kSsrcAndName) {
        return std::unexpected{Error::BadApp};
    }
    App a{
        .subtype = subtype, .ssrc = be32(body, 0), .name = {}, .data = body.subspan(kSsrcAndName)};
    std::ranges::copy(body.subspan(kWord, a.name.size()), a.name.begin());
    return a;
}

std::expected<RtcpPacket, Error> transport_feedback(std::uint8_t format, std::uint32_t sender,
                                                    std::uint32_t media,
                                                    std::span<const std::byte> fci) noexcept {
    if (format == kGenericNack) {
        if (fci.empty() || fci.size() % WireFormat<NackItem>::kSize != 0) {
            return std::unexpected{Error::BadFeedback};
        }
        return Nack{.sender_ssrc = sender, .media_ssrc = media, .items = Entries<NackItem>{fci}};
    }
    if (format == kTransportCc) {
        // Base sequence, status count, 24-bit reference time, feedback packet count.
        constexpr std::size_t kFixed = 8;
        if (fci.size() < kFixed) {
            return std::unexpected{Error::BadFeedback};
        }
        return TransportCc{.sender_ssrc = sender,
                           .media_ssrc = media,
                           .base_sequence = be16(fci, 0),
                           .status_count = be16(fci, 2),
                           .reference_time = signed24(be24(fci, 4)),
                           .feedback_count = u8(fci, 7),
                           .chunks_and_deltas = fci.subspan(kFixed)};
    }
    return Feedback{.type = RtcpType::TransportFeedback,
                    .format = format,
                    .sender_ssrc = sender,
                    .media_ssrc = media,
                    .fci = fci};
}

bool is_remb(std::span<const std::byte> fci) noexcept {
    constexpr std::array<std::byte, 4> kRemb{std::byte{'R'}, std::byte{'E'}, std::byte{'M'},
                                             std::byte{'B'}};
    return fci.size() >= kRemb.size() && std::ranges::equal(fci.first(kRemb.size()), kRemb);
}

std::expected<RtcpPacket, Error> remb(std::uint32_t sender, std::uint32_t media,
                                      std::span<const std::byte> fci) noexcept {
    // "REMB", SSRC count, 6-bit exponent and 18-bit mantissa, then the SSRCs.
    constexpr std::size_t kFixed = 8;
    if (fci.size() < kFixed) {
        return std::unexpected{Error::BadFeedback};
    }
    const std::size_t ssrcs = kWord * u8(fci, 4);
    if (fci.size() - kFixed != ssrcs) {
        return std::unexpected{Error::BadFeedback};
    }
    const unsigned exponent = u8(fci, 5) >> 2U;
    const std::uint64_t mantissa = be24(fci, 5) & 0x3'FFFFU;
    if (mantissa != 0 && std::countl_zero(mantissa) < static_cast<int>(exponent)) {
        return std::unexpected{Error::BadFeedback};
    }
    return Remb{.sender_ssrc = sender,
                .media_ssrc = media,
                .bitrate = mantissa << exponent,
                .ssrcs = Entries<std::uint32_t>{fci.subspan(kFixed)}};
}

std::expected<RtcpPacket, Error> payload_feedback(std::uint8_t format, std::uint32_t sender,
                                                  std::uint32_t media,
                                                  std::span<const std::byte> fci) noexcept {
    if (format == kPli) {
        return Pli{.sender_ssrc = sender, .media_ssrc = media};
    }
    if (format == kFir) {
        if (fci.empty() || fci.size() % WireFormat<FirEntry>::kSize != 0) {
            return std::unexpected{Error::BadFeedback};
        }
        return Fir{.sender_ssrc = sender, .media_ssrc = media, .entries = Entries<FirEntry>{fci}};
    }
    if (format == kApplicationLayer && is_remb(fci)) {
        return remb(sender, media, fci);
    }
    return Feedback{.type = RtcpType::PayloadFeedback,
                    .format = format,
                    .sender_ssrc = sender,
                    .media_ssrc = media,
                    .fci = fci};
}

std::expected<RtcpPacket, Error> feedback(bool transport, std::uint8_t format,
                                          std::span<const std::byte> body) noexcept {
    if (body.size() < kFeedbackHeader) {
        return std::unexpected{Error::BadFeedback};
    }
    const std::uint32_t sender = be32(body, 0);
    const std::uint32_t media = be32(body, 4);
    const std::span<const std::byte> fci = body.subspan(kFeedbackHeader);
    return transport ? transport_feedback(format, sender, media, fci)
                     : payload_feedback(format, sender, media, fci);
}

std::expected<RtcpPacket, Error> packet(std::uint8_t type, std::uint8_t count,
                                        std::span<const std::byte> body) noexcept {
    switch (type) {
    case static_cast<std::uint8_t>(RtcpType::SenderReport):
        return reports(true, count, body);
    case static_cast<std::uint8_t>(RtcpType::ReceiverReport):
        return reports(false, count, body);
    case static_cast<std::uint8_t>(RtcpType::SourceDescription):
        return source_description(count, body);
    case static_cast<std::uint8_t>(RtcpType::Bye):
        return bye(count, body);
    case static_cast<std::uint8_t>(RtcpType::App):
        return app(count, body);
    case static_cast<std::uint8_t>(RtcpType::TransportFeedback):
        return feedback(true, count, body);
    case static_cast<std::uint8_t>(RtcpType::PayloadFeedback):
        return feedback(false, count, body);
    default:
        return UnknownRtcp{.type = type, .count = count, .body = body};
    }
}

} // namespace

ReportBlock WireFormat<ReportBlock>::read(std::span<const std::byte, kSize> b) noexcept {
    return ReportBlock{.ssrc = be32(b, 0),
                       .fraction_lost = u8(b, 4),
                       .cumulative_lost = signed24(be24(b, 5)),
                       .highest_sequence = be32(b, 8),
                       .jitter = be32(b, 12),
                       .last_sender_report = be32(b, 16),
                       .delay_since_last_sender_report = be32(b, 20)};
}

NackItem WireFormat<NackItem>::read(std::span<const std::byte, kSize> b) noexcept {
    return NackItem{.packet_id = be16(b, 0), .lost_bitmask = be16(b, 2)};
}

FirEntry WireFormat<FirEntry>::read(std::span<const std::byte, kSize> b) noexcept {
    return FirEntry{.ssrc = be32(b, 0), .sequence = u8(b, 4)};
}

std::optional<SdesItem> SdesItems::stop(bool malformed) noexcept {
    malformed_ = malformed;
    chunks_ = {};
    offset_ = 0;
    chunks_left_ = 0;
    return std::nullopt;
}

// RFC 3550 section 6.5: each chunk is an SSRC, items (type, length, text), and a null END
// item padded with nulls to the next word.
std::optional<SdesItem> SdesItems::next() noexcept {
    constexpr std::uint8_t kEnd = 0;
    while (true) {
        if (!ssrc_) {
            if (chunks_left_ == 0) {
                return stop(offset_ != chunks_.size());
            }
            if (chunks_.size() - offset_ < kWord) {
                return stop(true);
            }
            ssrc_ = be32(chunks_, offset_);
            offset_ += kWord;
            --chunks_left_;
        }
        if (offset_ == chunks_.size()) {
            return stop(true);
        }
        const std::uint8_t type = u8(chunks_, offset_);
        if (type == kEnd) {
            // The next chunk starts at the word after the END item.
            offset_ = (offset_ + kWord) & ~(kWord - 1);
            if (offset_ > chunks_.size()) {
                return stop(true);
            }
            ssrc_.reset();
            continue;
        }
        if (chunks_.size() - offset_ < 2 ||
            chunks_.size() - offset_ - 2 < u8(chunks_, offset_ + 1)) {
            return stop(true);
        }
        const std::size_t length = u8(chunks_, offset_ + 1);
        const SdesItem item{.ssrc = *ssrc_,
                            .type = static_cast<SdesType>(type),
                            .value = chunks_.subspan(offset_ + 2, length)};
        offset_ += 2 + length;
        return item;
    }
}

std::optional<std::expected<RtcpPacket, Error>> CompoundReader::next() noexcept {
    if (error_) {
        return std::unexpected{*error_};
    }
    if (rest_.empty()) {
        return std::nullopt;
    }
    const auto fail = [&](Error e) {
        error_ = e;
        rest_ = {};
        return std::unexpected{e};
    };
    if (rest_.size() < kHeader) {
        return fail(Error::Truncated);
    }
    const std::uint8_t b0 = u8(rest_, 0);
    if ((b0 & kVersionBits) != kVersion2) {
        return fail(Error::BadVersion);
    }
    // RFC 3550 section 6.4.1: the length is in words, minus one, header included.
    const std::size_t length = kWord * (std::size_t{be16(rest_, 2)} + 1);
    if (rest_.size() < length) {
        return fail(Error::Truncated);
    }
    const std::uint8_t type = u8(rest_, 1);
    std::span<const std::byte> body = rest_.subspan(kHeader, length - kHeader);
    rest_ = rest_.subspan(length);
    if ((b0 & kPaddingBit) != 0) {
        // Only the last packet of a compound may be padded (RFC 3550 section 6.4.1), and the
        // last byte counts the padding, itself included.
        const std::uint8_t padding = body.empty() ? 0 : u8(body, body.size() - 1);
        if (!rest_.empty() || padding == 0 || padding > body.size()) {
            return fail(Error::BadPadding);
        }
        body = body.first(body.size() - padding);
    }
    auto result = packet(type, b0 & kCountBits, body);
    if (!result) {
        return fail(result.error());
    }
    return result;
}

} // namespace codec::rtp
