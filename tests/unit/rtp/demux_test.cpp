#include "codec/rtp/demux.hpp"

#include "wire.hpp"

#include <gtest/gtest.h>

namespace {

using codec::rtp::classify;
using codec::rtp::PacketKind;
using ulw::test::bytes_of;

TEST(Demux, SecondBytesOfRtcpTypesAreRtcp) {
    for (const unsigned type : {192U, 200U, 201U, 205U, 206U, 223U}) {
        EXPECT_EQ(classify(bytes_of({0x80, type, 0, 1})), PacketKind::Rtcp) << type;
    }
}

TEST(Demux, EverythingElseUnderVersionTwoIsRtp) {
    // 191 is payload type 63 with the marker set, 224 is 96 with it: both outside RTCP's range.
    for (const unsigned second : {0U, 96U, 111U, 127U, 191U, 224U, 0xE0U, 255U}) {
        EXPECT_EQ(classify(bytes_of({0x80, second})), PacketKind::Rtp) << second;
    }
    // Padding, extension and CSRC bits do not change the verdict.
    EXPECT_EQ(classify(bytes_of({0xBF, 96})), PacketKind::Rtp);
    EXPECT_EQ(classify(bytes_of({0xA1, 200})), PacketKind::Rtcp);
}

TEST(Demux, StunDtlsAndShortDatagramsAreNeither) {
    // RFC 7983: STUN starts 0-3, DTLS 20-63, and neither has version bits 10.
    EXPECT_EQ(classify(bytes_of({0x00, 0x01, 0x00, 0x00})), PacketKind::Other);
    EXPECT_EQ(classify(bytes_of({0x16, 0xFE, 0xFD})), PacketKind::Other);
    EXPECT_EQ(classify(bytes_of({0x40, 200})), PacketKind::Other);
    EXPECT_EQ(classify(bytes_of({0xC0, 96})), PacketKind::Other);
    EXPECT_EQ(classify(bytes_of({0x80})), PacketKind::Other);
    EXPECT_EQ(classify({}), PacketKind::Other);
}

} // namespace
