#include "codec/sdp/parser.hpp"
#include "codec/sdp/serializer.hpp"
#include "codec/sdp/session.hpp"

#include "text.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>

namespace {

using codec::sdp::Attribute;
using codec::sdp::Bandwidth;
using codec::sdp::Candidate;
using codec::sdp::Connection;
using codec::sdp::Direction;
using codec::sdp::EndOfCandidates;
using codec::sdp::Extmap;
using codec::sdp::Fingerprint;
using codec::sdp::Fmtp;
using codec::sdp::Group;
using codec::sdp::IceOptions;
using codec::sdp::IcePwd;
using codec::sdp::IceUfrag;
using codec::sdp::MediaDescription;
using codec::sdp::Mid;
using codec::sdp::Msid;
using codec::sdp::Origin;
using codec::sdp::Rid;
using codec::sdp::RidDirection;
using codec::sdp::RtcpFeedback;
using codec::sdp::RtcpMux;
using codec::sdp::RtcpRsize;
using codec::sdp::Rtpmap;
using codec::sdp::serialize;
using codec::sdp::Session;
using codec::sdp::Setup;
using codec::sdp::SetupRole;
using codec::sdp::Simulcast;
using codec::sdp::Ssrc;
using codec::sdp::SsrcGroup;
using codec::sdp::Timing;
using codec::sdp::UnknownAttribute;
using ulw::test::kFingerprint;
using ulw::test::Parsed;

// An answer as the SFU side would build it: every typed attribute once, written in RFC 8866
// order whatever order the fields were filled in.
Session answer() {
    Session s;
    s.origin = Origin{.username = "ulw",
                      .session_id = 4611731400430051336ULL,
                      .session_version = 2,
                      .network_type = "IN",
                      .address_type = "IP4",
                      .address = "0.0.0.0"};
    s.name = "-";
    s.timings.push_back(Timing{.start = 0, .stop = 0, .repeats = {}});
    s.attributes = {
        Attribute{.value = Group{.semantics = "BUNDLE", .tags = {"0"}}},
        Attribute{.value = IceUfrag{"Ab+/"}},
        Attribute{.value = IcePwd{"0123456789abcdefghijkl"}},
        Attribute{.value = IceOptions{.options = {"trickle"}}},
        Attribute{.value = Fingerprint{.algorithm = "sha-256", .value = kFingerprint.substr(8)}},
        Attribute{.value = Setup{SetupRole::Passive}},
        Attribute{.value = UnknownAttribute{.name = "ice-lite", .value = {}}},
    };
    MediaDescription video{.media = "video",
                           .port = 9,
                           .port_count = {},
                           .protocol = "UDP/TLS/RTP/SAVPF",
                           .formats = {"96", "97"},
                           .information = {},
                           .connections = {Connection{
                               .network_type = "IN", .address_type = "IP4", .address = "0.0.0.0"}},
                           .bandwidths = {Bandwidth{.type = "AS", .value = 2500}},
                           .key = {},
                           .attributes = {},
                           .line = 0};
    video.attributes = {
        Attribute{.value = Mid{"0"}},
        Attribute{.value = Direction::RecvOnly},
        Attribute{.value = Extmap{.id = 3,
                                  .direction = {},
                                  .uri = "urn:ietf:params:rtp-hdrext:sdes:mid",
                                  .attributes = {}}},
        Attribute{.value = RtcpMux{}},
        Attribute{.value = RtcpRsize{}},
        Attribute{
            .value =
                Rtpmap{.payload_type = 96, .encoding = "VP8", .clock_rate = 90000, .channels = {}}},
        Attribute{.value = RtcpFeedback{.payload_type = 96, .type = "nack", .parameter = "pli"}},
        Attribute{
            .value =
                Rtpmap{.payload_type = 97, .encoding = "rtx", .clock_rate = 90000, .channels = {}}},
        Attribute{.value = Fmtp{.format = "97", .parameters = "apt=96"}},
        Attribute{.value = Msid{.stream = "-", .track = "cam"}},
        Attribute{.value = Ssrc{.ssrc = 1, .attribute = "cname", .value = "sfu"}},
        Attribute{.value = Ssrc{.ssrc = 2, .attribute = "cname", .value = "sfu"}},
        Attribute{.value = SsrcGroup{.semantics = "FID", .ssrcs = {1, 2}}},
        Attribute{.value = Rid{.id = "q", .direction = RidDirection::Recv, .restrictions = {}}},
        Attribute{
            .value =
                Rid{.id = "f", .direction = RidDirection::Recv, .restrictions = "max-width=1280"}},
        Attribute{.value = Simulcast{.first = {.direction = RidDirection::Recv, .streams = "q;~f"},
                                     .second = {}}},
        Attribute{.value = Candidate{.foundation = "1",
                                     .component = 1,
                                     .transport = "udp",
                                     .priority = 2130706431,
                                     .address = "198.51.100.7",
                                     .port = 3478,
                                     .type = "relay",
                                     .related_address = "0.0.0.0",
                                     .related_port = 0,
                                     .extensions = {}}},
        Attribute{.value = EndOfCandidates{}},
    };
    s.media.push_back(video);
    return s;
}

TEST(SdpSerializer, WritesEveryLineInRfcOrderWithCrlf) {
    const std::string expected = "v=0\r\n"
                                 "o=ulw 4611731400430051336 2 IN IP4 0.0.0.0\r\n"
                                 "s=-\r\n"
                                 "t=0 0\r\n"
                                 "a=group:BUNDLE 0\r\n"
                                 "a=ice-ufrag:Ab+/\r\n"
                                 "a=ice-pwd:0123456789abcdefghijkl\r\n"
                                 "a=ice-options:trickle\r\n"
                                 "a=fingerprint:sha-256 05:B8:BE:BA:8C:BF:94:93:B2:2B:40:B7:B5:"
                                 "12:F3:18:02:5D:A5:C4:69:AF:F8:52:13:E1:60:0E:47:CB:7B:78\r\n"
                                 "a=setup:passive\r\n"
                                 "a=ice-lite\r\n"
                                 "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
                                 "c=IN IP4 0.0.0.0\r\n"
                                 "b=AS:2500\r\n"
                                 "a=mid:0\r\n"
                                 "a=recvonly\r\n"
                                 "a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
                                 "a=rtcp-mux\r\n"
                                 "a=rtcp-rsize\r\n"
                                 "a=rtpmap:96 VP8/90000\r\n"
                                 "a=rtcp-fb:96 nack pli\r\n"
                                 "a=rtpmap:97 rtx/90000\r\n"
                                 "a=fmtp:97 apt=96\r\n"
                                 "a=msid:- cam\r\n"
                                 "a=ssrc:1 cname:sfu\r\n"
                                 "a=ssrc:2 cname:sfu\r\n"
                                 "a=ssrc-group:FID 1 2\r\n"
                                 "a=rid:q recv\r\n"
                                 "a=rid:f recv max-width=1280\r\n"
                                 "a=simulcast:recv q;~f\r\n"
                                 "a=candidate:1 1 udp 2130706431 198.51.100.7 3478 typ relay "
                                 "raddr 0.0.0.0 rport 0\r\n"
                                 "a=end-of-candidates\r\n";
    EXPECT_EQ(serialize(answer()), expected);
}

TEST(SdpSerializer, WhatItWritesParsesBackToTheSameSession) {
    const Session built = answer();
    const Parsed p{serialize(built)};
    ASSERT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
    EXPECT_EQ(p.session(), built);
    EXPECT_EQ(p.session().media[0].attributes[5].line, 20U);
}

TEST(SdpSerializer, NumbersAtTheirLimitsAreWrittenInFull) {
    Session s = answer();
    s.origin.session_id = std::numeric_limits<std::uint64_t>::max();
    s.media[0].attributes[16].value = Candidate{.foundation = "f",
                                                .component = 256,
                                                .transport = "tcp",
                                                .priority = 2147483647,
                                                .address = "::1",
                                                .port = 65535,
                                                .type = "host",
                                                .related_address = {},
                                                .related_port = {},
                                                .extensions = "tcptype passive"};
    const std::string text = serialize(s);
    EXPECT_NE(text.find("o=ulw 18446744073709551615 2 "), std::string::npos);
    EXPECT_NE(text.find("a=candidate:f 256 tcp 2147483647 ::1 65535 typ host tcptype passive\r\n"),
              std::string::npos);
    const Parsed p{text};
    ASSERT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
    EXPECT_EQ(p.session(), s);
}

} // namespace
