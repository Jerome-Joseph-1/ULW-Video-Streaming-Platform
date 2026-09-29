#pragma once

#include "codec/rtp/entries.hpp"
#include "codec/rtp/error.hpp"
#include "core/util/lifetime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <variant>
#include <vector>

// RTCP (RFC 3550 section 6) and the RTP/AVPF feedback messages WebRTC sends (RFC 4585,
// RFC 5104, draft-alvestrand-rmcat-remb, draft-holmer-rmcat-transport-wide-cc-extensions), read
// in place: every span points into the datagram given to CompoundReader.
namespace codec::rtp {

enum class RtcpType : std::uint8_t {
    SenderReport = 200,
    ReceiverReport = 201,
    SourceDescription = 202,
    Bye = 203,
    App = 204,
    TransportFeedback = 205,
    PayloadFeedback = 206,
};

struct ReportBlock {
    std::uint32_t ssrc = 0;
    std::uint8_t fraction_lost = 0;
    // 24 bits, signed: duplicates can make it negative.
    std::int32_t cumulative_lost = 0;
    std::uint32_t highest_sequence = 0;
    std::uint32_t jitter = 0;
    std::uint32_t last_sender_report = 0;
    std::uint32_t delay_since_last_sender_report = 0;
};

template <> struct WireFormat<ReportBlock> {
    static constexpr std::size_t kSize = 24;
    [[nodiscard]] static ReportBlock read(std::span<const std::byte, kSize> bytes) noexcept;
};

struct SenderReport {
    std::uint32_t ssrc = 0;
    std::uint64_t ntp_timestamp = 0;
    std::uint32_t rtp_timestamp = 0;
    std::uint32_t packet_count = 0;
    std::uint32_t octet_count = 0;
    Entries<ReportBlock> reports;
    // Profile-specific extension after the report blocks.
    std::span<const std::byte> extension;
};

struct ReceiverReport {
    std::uint32_t ssrc = 0;
    Entries<ReportBlock> reports;
    std::span<const std::byte> extension;
};

// RFC 3550 section 6.5. Types past PRIV come through with their number.
enum class SdesType : std::uint8_t {
    Cname = 1,
    Name = 2,
    Email = 3,
    Phone = 4,
    Location = 5,
    Tool = 6,
    Note = 7,
    Private = 8,
};

struct SdesItem {
    std::uint32_t ssrc = 0;
    SdesType type = SdesType::Cname;
    std::span<const std::byte> value;
};

// The items of every chunk, in order, each with its chunk's SSRC.
class SdesItems {
public:
    SdesItems(std::span<const std::byte> chunks, std::uint8_t count) noexcept
        : chunks_(chunks), chunks_left_(count) {}

    [[nodiscard]] std::optional<SdesItem> next() noexcept;
    // The chunks did not add up to the packet: an item or a chunk ran past its end, or bytes
    // were left after the last chunk. The reader refuses such packets.
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }

private:
    std::optional<SdesItem> stop(bool malformed) noexcept;

    std::span<const std::byte> chunks_;
    std::size_t offset_ = 0;
    std::uint8_t chunks_left_;
    std::optional<std::uint32_t> ssrc_;
    bool malformed_ = false;
};

struct SourceDescription {
    std::uint8_t chunk_count = 0;
    std::span<const std::byte> chunks;

    [[nodiscard]] SdesItems items() const noexcept { return SdesItems{chunks, chunk_count}; }
};

struct Bye {
    Entries<std::uint32_t> ssrcs;
    std::optional<std::span<const std::byte>> reason;
};

struct App {
    std::uint8_t subtype = 0;
    std::uint32_t ssrc = 0;
    std::array<std::byte, 4> name{};
    std::span<const std::byte> data;
};

// RFC 4585 section 6.2.1.
struct NackItem {
    std::uint16_t packet_id = 0;
    // Bit i set: packet_id + i + 1 was lost too.
    std::uint16_t lost_bitmask = 0;
};

template <> struct WireFormat<NackItem> {
    static constexpr std::size_t kSize = 4;
    [[nodiscard]] static NackItem read(std::span<const std::byte, kSize> bytes) noexcept;
};

struct Nack {
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_ssrc = 0;
    Entries<NackItem> items;
};

// draft-holmer-rmcat-transport-wide-cc-extensions-01 section 3.1: the fixed fields. The status
// chunks and receive deltas that follow are left encoded.
struct TransportCc {
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_ssrc = 0;
    std::uint16_t base_sequence = 0;
    std::uint16_t status_count = 0;
    // 24 bits, signed, in multiples of 64 ms.
    std::int32_t reference_time = 0;
    std::uint8_t feedback_count = 0;
    std::span<const std::byte> chunks_and_deltas;
};

// RFC 4585 section 6.3.1.
struct Pli {
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_ssrc = 0;
};

// RFC 5104 section 4.3.1.
struct FirEntry {
    std::uint32_t ssrc = 0;
    std::uint8_t sequence = 0;
};

template <> struct WireFormat<FirEntry> {
    static constexpr std::size_t kSize = 8;
    [[nodiscard]] static FirEntry read(std::span<const std::byte, kSize> bytes) noexcept;
};

struct Fir {
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_ssrc = 0;
    Entries<FirEntry> entries;
};

// draft-alvestrand-rmcat-remb-03 section 2.2.
struct Remb {
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_ssrc = 0;
    std::uint64_t bitrate = 0;
    Entries<std::uint32_t> ssrcs;
};

// A feedback message of a format not typed above.
struct Feedback {
    RtcpType type = RtcpType::TransportFeedback;
    std::uint8_t format = 0;
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_ssrc = 0;
    std::span<const std::byte> fci;
};

// XR and anything else registered later.
struct UnknownRtcp {
    std::uint8_t type = 0;
    std::uint8_t count = 0;
    std::span<const std::byte> body;
};

using RtcpPacket = std::variant<SenderReport, ReceiverReport, SourceDescription, Bye, App, Nack,
                                TransportCc, Pli, Fir, Remb, Feedback, UnknownRtcp>;

// The packets of a compound RTCP datagram, in order. The first need not be a report: RFC 5506
// reduced-size RTCP, which browsers negotiate with a=rtcp-rsize, sends feedback on its own.
class CompoundReader {
public:
    explicit CompoundReader(std::span<const std::byte> datagram ULW_LIFETIMEBOUND) noexcept
        : rest_(datagram) {}
    explicit CompoundReader(std::vector<std::byte>&&) = delete;

    // The next packet; nothing past the last one. After an error the reader returns that error
    // again and reads no further.
    [[nodiscard]] std::optional<std::expected<RtcpPacket, Error>> next() noexcept;

private:
    std::span<const std::byte> rest_;
    std::optional<Error> error_;
};

} // namespace codec::rtp
