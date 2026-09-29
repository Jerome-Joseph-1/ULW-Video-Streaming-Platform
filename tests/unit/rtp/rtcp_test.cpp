#include "codec/rtp/error.hpp"
#include "codec/rtp/rtcp.hpp"

#include "wire.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace {

using codec::rtp::App;
using codec::rtp::Bye;
using codec::rtp::CompoundReader;
using codec::rtp::Error;
using codec::rtp::Feedback;
using codec::rtp::Fir;
using codec::rtp::Nack;
using codec::rtp::Pli;
using codec::rtp::ReceiverReport;
using codec::rtp::Remb;
using codec::rtp::ReportBlock;
using codec::rtp::RtcpPacket;
using codec::rtp::RtcpType;
using codec::rtp::SdesItems;
using codec::rtp::SdesType;
using codec::rtp::SenderReport;
using codec::rtp::SourceDescription;
using codec::rtp::TransportCc;
using codec::rtp::UnknownRtcp;
using ulw::test::as_text;
using ulw::test::bytes_of;
using ulw::test::Wire;

struct Read {
    std::vector<RtcpPacket> packets;
    std::optional<Error> error;
};

Read read_all(std::span<const std::byte> datagram) {
    Read out;
    CompoundReader reader{datagram};
    while (auto next = reader.next()) {
        if (!*next) {
            out.error = next->error();
            break;
        }
        out.packets.push_back(**next);
    }
    return out;
}

// The first packet, by value: its spans point into `datagram`, which the caller keeps.
template <typename T> T first(std::span<const std::byte> datagram) {
    return std::get<T>(read_all(datagram).packets.at(0));
}

std::vector<std::byte> copy(std::span<const std::byte> s) {
    return {s.begin(), s.end()};
}

// An SR with one report block, then an SDES with the sender's CNAME: what a sender's first
// compound packet looks like.
Wire sender_report_and_cname() {
    Wire w;
    w.rtcp_header(1, 200, 48)
        .u32(0x11111111)
        .u64(0xE000000180000000ULL)
        .u32(1234)
        .u32(10)
        .u32(1000)
        .u32(0x22222222)
        .u8(0x40)
        .u24(0xFFFFFE)
        .u32(0x00010005)
        .u32(7)
        .u32(0xABCD0000)
        .u32(0x00018000);
    w.rtcp_header(1, 202, 12).u32(0x11111111).raw({1, 4}).text("abcd").raw({0, 0});
    return w;
}

TEST(Rtcp, ReadsASenderReportAndItsReportBlock) {
    const auto wire = sender_report_and_cname().take();
    const Read r = read_all(wire);
    ASSERT_FALSE(r.error.has_value());
    ASSERT_EQ(r.packets.size(), 2U);
    const auto& sr = std::get<SenderReport>(r.packets[0]);
    EXPECT_EQ(sr.ssrc, 0x11111111U);
    EXPECT_EQ(sr.ntp_timestamp, 0xE000000180000000ULL);
    EXPECT_EQ(sr.rtp_timestamp, 1234U);
    EXPECT_EQ(sr.packet_count, 10U);
    EXPECT_EQ(sr.octet_count, 1000U);
    EXPECT_TRUE(sr.extension.empty());
    ASSERT_EQ(sr.reports.size(), 1U);
    const ReportBlock b = sr.reports[0];
    EXPECT_EQ(b.ssrc, 0x22222222U);
    EXPECT_EQ(b.fraction_lost, 0x40);
    EXPECT_EQ(b.cumulative_lost, -2);
    EXPECT_EQ(b.highest_sequence, 0x00010005U);
    EXPECT_EQ(b.jitter, 7U);
    EXPECT_EQ(b.last_sender_report, 0xABCD0000U);
    EXPECT_EQ(b.delay_since_last_sender_report, 0x00018000U);
}

TEST(Rtcp, ReadsSdesItemsAcrossChunks) {
    Wire w;
    // Chunk 1: CNAME "x", NAME "yz", END at byte 11. Chunk 2: no items, END and padding.
    w.rtcp_header(2, 202, 20)
        .u32(0xA)
        .raw({1, 1, 'x', 2, 2, 'y', 'z', 0})
        .u32(0xB)
        .raw({0, 0, 0, 0});
    const Read r = read_all(w.bytes());
    ASSERT_FALSE(r.error.has_value());
    const auto& sdes = std::get<SourceDescription>(r.packets.at(0));
    EXPECT_EQ(sdes.chunk_count, 2);
    SdesItems items = sdes.items();
    const auto first = items.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->ssrc, 0xAU);
    EXPECT_EQ(first->type, SdesType::Cname);
    EXPECT_EQ(as_text(first->value), "x");
    const auto second = items.next();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->type, SdesType::Name);
    EXPECT_EQ(as_text(second->value), "yz");
    EXPECT_FALSE(items.next().has_value());
    EXPECT_FALSE(items.malformed());
}

TEST(Rtcp, RefusesSdesChunksThatDoNotAddUp) {
    // An item whose length runs past the packet.
    EXPECT_EQ(read_all(Wire{}.rtcp_header(1, 202, 8).u32(1).raw({1, 16, 'a', 'b'}).take()).error,
              Error::BadSourceDescription);
    // Two chunks announced, one present.
    EXPECT_EQ(read_all(Wire{}.rtcp_header(2, 202, 8).u32(1).raw({0, 0, 0, 0}).take()).error,
              Error::BadSourceDescription);
    // A chunk without an END item.
    EXPECT_EQ(read_all(Wire{}.rtcp_header(1, 202, 8).u32(1).raw({1, 2, 'a', 'b'}).take()).error,
              Error::BadSourceDescription);
    // Bytes left after the last chunk.
    EXPECT_EQ(read_all(Wire{}.rtcp_header(1, 202, 12).u32(1).raw({0, 0, 0, 0}).u32(2).take()).error,
              Error::BadSourceDescription);
}

TEST(Rtcp, ReadsByeWithItsReason) {
    Wire w;
    w.rtcp_header(2, 203, 16).u32(5).u32(6).u8(5).text("gone!").raw({0, 0});
    const Read r = read_all(w.bytes());
    ASSERT_FALSE(r.error.has_value());
    const auto& bye = std::get<Bye>(r.packets.at(0));
    ASSERT_EQ(bye.ssrcs.size(), 2U);
    EXPECT_EQ(bye.ssrcs[0], 5U);
    EXPECT_EQ(bye.ssrcs[1], 6U);
    ASSERT_TRUE(bye.reason.has_value());
    EXPECT_EQ(as_text(*bye.reason), "gone!");
}

TEST(Rtcp, RefusesByeWhoseCountOrReasonOverruns) {
    EXPECT_EQ(read_all(Wire{}.rtcp_header(3, 203, 8).u32(1).u32(2).take()).error, Error::BadBye);
    EXPECT_EQ(read_all(Wire{}.rtcp_header(1, 203, 8).u32(1).u8(9).text("abc").take()).error,
              Error::BadBye);
}

TEST(Rtcp, ReadsApp) {
    Wire w;
    w.rtcp_header(3, 204, 12).u32(0x77).text("ULWx").raw({1, 2, 3, 4});
    const Read r = read_all(w.bytes());
    const auto& app = std::get<App>(r.packets.at(0));
    EXPECT_EQ(app.subtype, 3);
    EXPECT_EQ(app.ssrc, 0x77U);
    EXPECT_EQ(as_text(app.name), "ULWx");
    EXPECT_EQ(copy(app.data), bytes_of({1, 2, 3, 4}));
    EXPECT_EQ(read_all(Wire{}.rtcp_header(0, 204, 4).u32(1).take()).error, Error::BadApp);
}

TEST(Rtcp, ReadsGenericNack) {
    Wire w;
    w.rtcp_header(1, 205, 16).u32(1).u32(2).u16(100).u16(0x0005).u16(200).u16(0);
    const auto nack = first<Nack>(w.bytes());
    EXPECT_EQ(nack.sender_ssrc, 1U);
    EXPECT_EQ(nack.media_ssrc, 2U);
    ASSERT_EQ(nack.items.size(), 2U);
    EXPECT_EQ(nack.items[0].packet_id, 100);
    EXPECT_EQ(nack.items[0].lost_bitmask, 0x0005);
    EXPECT_EQ(nack.items[1].packet_id, 200);
    EXPECT_EQ(read_all(Wire{}.rtcp_header(1, 205, 8).u32(1).u32(2).take()).error,
              Error::BadFeedback);
}

TEST(Rtcp, ReadsTheFixedFieldsOfTransportWideFeedback) {
    Wire w;
    w.rtcp_header(15, 205, 20)
        .u32(1)
        .u32(2)
        .u16(7)
        .u16(3)
        .u24(0xFFFFFF)
        .u8(9)
        .raw({0x20, 0x03, 0x04, 0x04});
    const auto cc = first<TransportCc>(w.bytes());
    EXPECT_EQ(cc.base_sequence, 7);
    EXPECT_EQ(cc.status_count, 3);
    EXPECT_EQ(cc.reference_time, -1);
    EXPECT_EQ(cc.feedback_count, 9);
    EXPECT_EQ(copy(cc.chunks_and_deltas), bytes_of({0x20, 0x03, 0x04, 0x04}));
    EXPECT_EQ(read_all(Wire{}.rtcp_header(15, 205, 12).u32(1).u32(2).u32(3).take()).error,
              Error::BadFeedback);
}

TEST(Rtcp, ReadsPliAndFir) {
    Wire w;
    w.rtcp_header(1, 206, 8).u32(1).u32(2);
    w.rtcp_header(4, 206, 16).u32(1).u32(0).u32(0x33).u8(5).raw({0, 0, 0});
    const Read r = read_all(w.bytes());
    ASSERT_FALSE(r.error.has_value());
    const auto& pli = std::get<Pli>(r.packets.at(0));
    EXPECT_EQ(pli.media_ssrc, 2U);
    const auto& fir = std::get<Fir>(r.packets.at(1));
    ASSERT_EQ(fir.entries.size(), 1U);
    EXPECT_EQ(fir.entries[0].ssrc, 0x33U);
    EXPECT_EQ(fir.entries[0].sequence, 5);
    EXPECT_EQ(read_all(Wire{}.rtcp_header(4, 206, 12).u32(1).u32(0).u32(3).take()).error,
              Error::BadFeedback);
}

Wire remb(unsigned exponent, std::uint32_t mantissa, unsigned ssrcs) {
    Wire w;
    w.rtcp_header(15, 206, 16 + (4 * ssrcs))
        .u32(1)
        .u32(0)
        .text("REMB")
        .u8(ssrcs)
        .u24((exponent << 18U) | mantissa);
    for (unsigned i = 0; i < ssrcs; ++i) {
        w.u32(0x100 + i);
    }
    return w;
}

TEST(Rtcp, ReadsRembBitrateAndSsrcs) {
    const auto wire = remb(2, 75000, 2).take();
    const auto r = first<Remb>(wire);
    EXPECT_EQ(r.bitrate, 300000U);
    ASSERT_EQ(r.ssrcs.size(), 2U);
    EXPECT_EQ(r.ssrcs[1], 0x101U);
    // The largest bitrate that fits in 64 bits.
    const auto largest = remb(46, 0x3FFFF, 0).take();
    const auto max = first<Remb>(largest);
    EXPECT_EQ(max.bitrate, std::uint64_t{0x3FFFF} << 46U);
}

TEST(Rtcp, RefusesRembThatOverflowsOrMiscountsItsSsrcs) {
    EXPECT_EQ(read_all(remb(47, 0x3FFFF, 0).take()).error, Error::BadFeedback);
    EXPECT_EQ(read_all(remb(63, 3, 0).take()).error, Error::BadFeedback);
    // The SSRC count, after the header, the two SSRCs and "REMB".
    auto wire = remb(2, 75000, 2).take();
    wire.at(16) = std::byte{3};
    EXPECT_EQ(read_all(wire).error, Error::BadFeedback);
}

TEST(Rtcp, OtherFeedbackFormatsComeThroughUntyped) {
    Wire w;
    w.rtcp_header(3, 205, 12).u32(1).u32(2).u32(0xFEED);
    w.rtcp_header(15, 206, 12).u32(1).u32(2).text("ABCD");
    const Read r = read_all(w.bytes());
    ASSERT_FALSE(r.error.has_value());
    const auto& tmmbr = std::get<Feedback>(r.packets.at(0));
    EXPECT_EQ(tmmbr.type, RtcpType::TransportFeedback);
    EXPECT_EQ(tmmbr.format, 3);
    EXPECT_EQ(copy(tmmbr.fci), bytes_of({0, 0, 0xFE, 0xED}));
    const auto& afb = std::get<Feedback>(r.packets.at(1));
    EXPECT_EQ(afb.type, RtcpType::PayloadFeedback);
    EXPECT_EQ(afb.format, 15);
}

TEST(Rtcp, UnknownPacketTypesComeThroughWithTheirBody) {
    Wire w;
    w.rtcp_header(0, 207, 8).u32(1).u32(2);
    const auto xr = first<UnknownRtcp>(w.bytes());
    EXPECT_EQ(xr.type, 207);
    EXPECT_EQ(xr.body.size(), 8U);
}

TEST(Rtcp, ReceiverReportKeepsItsProfileExtension) {
    Wire w;
    w.rtcp_header(0, 201, 8).u32(9).raw({1, 2, 3, 4});
    const auto rr = first<ReceiverReport>(w.bytes());
    EXPECT_EQ(rr.ssrc, 9U);
    EXPECT_TRUE(rr.reports.empty());
    EXPECT_EQ(copy(rr.extension), bytes_of({1, 2, 3, 4}));
}

TEST(Rtcp, RefusesReportCountsThePacketCannotHold) {
    EXPECT_EQ(read_all(Wire{}.rtcp_header(1, 201, 8).u32(9).u32(0).take()).error,
              Error::BadReportCount);
    // Shorter than the sender info.
    EXPECT_EQ(read_all(Wire{}.rtcp_header(0, 200, 20).u32(1).u64(2).u32(3).u32(4).take()).error,
              Error::Truncated);
}

TEST(Rtcp, PaddingIsStrippedFromTheLastPacketOnly) {
    Wire last;
    last.rtcp_header(0, 201, 8, true).u32(9).raw({0, 0, 0, 4});
    const auto rr = first<ReceiverReport>(last.bytes());
    EXPECT_TRUE(rr.extension.empty());

    Wire first;
    first.rtcp_header(0, 201, 8, true).u32(9).raw({0, 0, 0, 4});
    first.rtcp_header(0, 201, 4).u32(10);
    EXPECT_EQ(read_all(first.bytes()).error, Error::BadPadding);

    EXPECT_EQ(read_all(Wire{}.rtcp_header(0, 201, 4, true).u32(0).take()).error, Error::BadPadding);
    EXPECT_EQ(read_all(Wire{}.rtcp_header(0, 201, 4, true).u32(9).take()).error, Error::BadPadding);
}

TEST(Rtcp, ErrorsStopTheReaderForGood) {
    Wire w;
    w.rtcp_header(0, 201, 4).u32(1);
    w.u8(0x40).u8(201).u16(1).u32(2);
    w.rtcp_header(0, 201, 4).u32(3);
    CompoundReader reader{w.bytes()};
    ASSERT_TRUE(reader.next().value().has_value());
    EXPECT_EQ(reader.next().value().error(), Error::BadVersion);
    EXPECT_EQ(reader.next().value().error(), Error::BadVersion);
}

TEST(Rtcp, EveryPrefixYieldsTheWholePacketsItHoldsThenAnError) {
    Wire w = sender_report_and_cname();
    w.rtcp_header(1, 205, 12).u32(1).u32(2).u16(100).u16(0);
    const auto whole = w.take();
    // Packet boundaries: SR (52 bytes), SDES (16), NACK (16).
    const std::vector<std::size_t> ends{52, 68, 84};
    ASSERT_EQ(whole.size(), ends.back());
    for (std::size_t n = 0; n <= whole.size(); ++n) {
        const std::vector<std::byte> prefix(whole.begin(), whole.begin() + static_cast<long>(n));
        const Read r = read_all(prefix);
        std::size_t complete = 0;
        while (complete < ends.size() && ends[complete] <= n) {
            ++complete;
        }
        EXPECT_EQ(r.packets.size(), complete) << n;
        const bool at_boundary = n == 0 || (complete > 0 && ends[complete - 1] == n);
        EXPECT_EQ(r.error.has_value(), !at_boundary) << n;
    }
}

} // namespace
