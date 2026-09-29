// One test per typed attribute: what it reads into, and the malformed forms it refuses. Each
// line is appended to the media section of minimal() (line 14), or put at session level
// before the BUNDLE group (line 5).

#include "codec/sdp/error.hpp"
#include "codec/sdp/serializer.hpp"
#include "codec/sdp/session.hpp"

#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <gtest/gtest.h>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using codec::sdp::AttributeValue;
using codec::sdp::Candidate;
using codec::sdp::Direction;
using codec::sdp::EndOfCandidates;
using codec::sdp::ErrorCode;
using codec::sdp::Extmap;
using codec::sdp::Fingerprint;
using codec::sdp::Fmtp;
using codec::sdp::Group;
using codec::sdp::IceOptions;
using codec::sdp::IcePwd;
using codec::sdp::IceUfrag;
using codec::sdp::Mid;
using codec::sdp::Msid;
using codec::sdp::Rid;
using codec::sdp::RidDirection;
using codec::sdp::RtcpFeedback;
using codec::sdp::RtcpMux;
using codec::sdp::RtcpRsize;
using codec::sdp::Rtpmap;
using codec::sdp::serialize;
using codec::sdp::SetupRole;
using codec::sdp::Simulcast;
using codec::sdp::SimulcastStreams;
using codec::sdp::Ssrc;
using codec::sdp::SsrcGroup;
using ulw::test::error_at;
using ulw::test::inserted;
using ulw::test::Lines;
using ulw::test::minimal;
using ulw::test::Parsed;
using ulw::test::replaced;
using ulw::test::with;

Lines in_media(std::initializer_list<std::string_view> lines) {
    Lines l = minimal();
    for (const std::string_view line : lines) {
        l.emplace_back(line);
    }
    return l;
}

// The typed value of the section's last attribute, after checking that the whole description
// parsed and serializes back to its text.
const AttributeValue& last(const Parsed& p) {
    EXPECT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
    EXPECT_EQ(serialize(p.session()), p.source());
    return p.session().media.at(0).attributes.back().value;
}

const AttributeValue& session_first(const Parsed& p) {
    EXPECT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
    EXPECT_EQ(serialize(p.session()), p.source());
    return p.session().attributes.at(0).value;
}

void expect_refused_in_media(std::initializer_list<std::string_view> bad) {
    for (const std::string_view line : bad) {
        EXPECT_EQ(Parsed(with(minimal(), line)).error(), error_at(ErrorCode::BadAttribute, 14))
            << line;
    }
}

TEST(SdpAttributes, Rtpmap) {
    EXPECT_EQ(last(Parsed{in_media({"a=rtpmap:96 VP8/90000"})}),
              (AttributeValue{Rtpmap{
                  .payload_type = 96, .encoding = "VP8", .clock_rate = 90000, .channels = {}}}));
    EXPECT_EQ(last(Parsed{in_media({"a=rtpmap:0 PCMU/8000"})}),
              (AttributeValue{Rtpmap{
                  .payload_type = 0, .encoding = "PCMU", .clock_rate = 8000, .channels = {}}}));
    EXPECT_EQ(last(Parsed{in_media({"a=rtpmap:97 L16/44100/2"})}),
              (AttributeValue{Rtpmap{
                  .payload_type = 97, .encoding = "L16", .clock_rate = 44100, .channels = 2}}));
    expect_refused_in_media({"a=rtpmap:96", "a=rtpmap", "a=rtpmap:96 VP8", "a=rtpmap:96 VP8/",
                             "a=rtpmap:096 VP8/90000", "a=rtpmap:128 x/1",
                             "a=rtpmap:96 VP8/90000/2/3", "a=rtpmap:96  VP8/90000",
                             "a=rtpmap:96 VP8/0", "a=rtpmap:96 VP8/90000/0",
                             "a=rtpmap:96 VP8/90000 ", "a=rtpmap:96 /90000"});
}

TEST(SdpAttributes, Fmtp) {
    EXPECT_EQ(last(Parsed{in_media({"a=fmtp:111 minptime=10;useinbandfec=1"})}),
              (AttributeValue{Fmtp{.format = "111", .parameters = "minptime=10;useinbandfec=1"}}));
    // Parameters are the codec's business, spaces included.
    EXPECT_EQ(last(Parsed{in_media({"a=fmtp:111 a=1; b=2"})}),
              (AttributeValue{Fmtp{.format = "111", .parameters = "a=1; b=2"}}));
    expect_refused_in_media({"a=fmtp:111", "a=fmtp:111 ", "a=fmtp: x"});
}

TEST(SdpAttributes, RtcpFeedback) {
    EXPECT_EQ(
        last(Parsed{in_media({"a=rtcp-fb:96 nack pli"})}),
        (AttributeValue{RtcpFeedback{.payload_type = 96, .type = "nack", .parameter = "pli"}}));
    EXPECT_EQ(last(Parsed{in_media({"a=rtcp-fb:* transport-cc"})}),
              (AttributeValue{RtcpFeedback{
                  .payload_type = std::nullopt, .type = "transport-cc", .parameter = {}}}));
    EXPECT_EQ(
        last(Parsed{in_media({"a=rtcp-fb:111 trr-int 100"})}),
        (AttributeValue{RtcpFeedback{.payload_type = 111, .type = "trr-int", .parameter = "100"}}));
    expect_refused_in_media(
        {"a=rtcp-fb:96", "a=rtcp-fb:96 nack ", "a=rtcp-fb:x nack", "a=rtcp-fb:96  nack"});
}

TEST(SdpAttributes, Extmap) {
    EXPECT_EQ(last(Parsed{in_media({"a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level"})}),
              (AttributeValue{Extmap{.id = 1,
                                     .direction = {},
                                     .uri = "urn:ietf:params:rtp-hdrext:ssrc-audio-level",
                                     .attributes = {}}}));
    EXPECT_EQ(
        last(Parsed{in_media({"a=extmap:2/recvonly urn:x vad=on x"})}),
        (AttributeValue{Extmap{
            .id = 2, .direction = Direction::RecvOnly, .uri = "urn:x", .attributes = "vad=on x"}}));
    EXPECT_EQ(std::get<Extmap>(last(Parsed{in_media({"a=extmap:4096 urn:x"})})).id, 4096);
    EXPECT_EQ(std::get<Extmap>(last(Parsed{in_media({"a=extmap:255 urn:x"})})).id, 255);
    expect_refused_in_media({"a=extmap:0 urn:x", "a=extmap:256 urn:x", "a=extmap:4352 urn:x",
                             "a=extmap:1/both urn:x", "a=extmap:1", "a=extmap:01 urn:x",
                             "a=extmap:1 urn:x "});
    // Also allowed for the whole session (RFC 8285 section 8).
    EXPECT_EQ(
        std::get<Extmap>(session_first(Parsed{inserted(minimal(), 5, "a=extmap:3 urn:y")})).id, 3);
}

TEST(SdpAttributes, Mid) {
    EXPECT_EQ(std::get<Mid>(Parsed{minimal()}.session().media[0].attributes[4].value).tag, "0");
    // Refused at session level, and when it is not a token.
    EXPECT_EQ(Parsed(inserted(minimal(), 5, "a=mid:x")).error(),
              error_at(ErrorCode::MisplacedAttribute, 5));
    expect_refused_in_media({"a=mid:", "a=mid:a b", "a=mid"});
}

TEST(SdpAttributes, Group) {
    EXPECT_EQ(session_first(Parsed{inserted(minimal(), 5, "a=group:LS 0")}),
              (AttributeValue{Group{.semantics = "LS", .tags = {"0"}}}));
    EXPECT_EQ(session_first(Parsed{inserted(minimal(), 5, "a=group:FEC")}),
              (AttributeValue{Group{.semantics = "FEC", .tags = {}}}));
    EXPECT_EQ(Parsed(inserted(minimal(), 5, "a=group:LS  0")).error(),
              error_at(ErrorCode::BadAttribute, 5));
    EXPECT_EQ(Parsed(with(minimal(), "a=group:LS 0")).error(),
              error_at(ErrorCode::MisplacedAttribute, 14));
}

TEST(SdpAttributes, Msid) {
    EXPECT_EQ(last(Parsed{in_media({"a=msid:stream track"})}),
              (AttributeValue{Msid{.stream = "stream", .track = "track"}}));
    EXPECT_EQ(last(Parsed{in_media({"a=msid:- track"})}),
              (AttributeValue{Msid{.stream = "-", .track = "track"}}));
    EXPECT_EQ(last(Parsed{in_media({"a=msid:stream"})}),
              (AttributeValue{Msid{.stream = "stream", .track = {}}}));
    // RFC 8830 section 2: at most 64 characters each.
    const std::string longest = "a=msid:" + std::string(64, 'x');
    EXPECT_TRUE(Parsed(with(minimal(), longest)).ok());
    expect_refused_in_media(
        {"a=msid:" + std::string(65, 'x'), "a=msid:a b c", "a=msid:a ", "a=msid:"});
}

TEST(SdpAttributes, Ssrc) {
    EXPECT_EQ(last(Parsed{in_media({"a=ssrc:1 cname:x"})}),
              (AttributeValue{Ssrc{.ssrc = 1, .attribute = "cname", .value = "x"}}));
    EXPECT_EQ(last(Parsed{in_media({"a=ssrc:4294967295 msid:a b"})}),
              (AttributeValue{Ssrc{.ssrc = 4294967295, .attribute = "msid", .value = "a b"}}));
    EXPECT_EQ(last(Parsed{in_media({"a=ssrc:7 label"})}),
              (AttributeValue{Ssrc{.ssrc = 7, .attribute = "label", .value = {}}}));
    expect_refused_in_media(
        {"a=ssrc:4294967296 cname:x", "a=ssrc:1", "a=ssrc:1 :x", "a=ssrc:x cname:y"});
}

TEST(SdpAttributes, SsrcGroup) {
    EXPECT_EQ(last(Parsed{in_media({"a=ssrc-group:FID 1 2"})}),
              (AttributeValue{SsrcGroup{.semantics = "FID", .ssrcs = {1, 2}}}));
    expect_refused_in_media({"a=ssrc-group:FID", "a=ssrc-group:FID 1 x", "a=ssrc-group:FID 1 "});
}

TEST(SdpAttributes, IceCredentialsAndOptions) {
    const Parsed p{minimal()};
    EXPECT_EQ(p.session().media[0].attributes[0].value, (AttributeValue{IceUfrag{"abcd"}}));
    EXPECT_EQ(p.session().media[0].attributes[1].value,
              (AttributeValue{IcePwd{"aaaaaaaaaaaaaaaaaaaaaa"}}));
    EXPECT_EQ(last(Parsed{in_media({"a=ice-options:trickle renomination"})}),
              (AttributeValue{IceOptions{.options = {"trickle", "renomination"}}}));
    // RFC 8839 section 5.4: ice-ufrag is 4 to 256 ice-chars, ice-pwd 22 to 256.
    expect_refused_in_media({"a=ice-ufrag:abc", "a=ice-ufrag:ab-d",
                             "a=ice-pwd:aaaaaaaaaaaaaaaaaaaaa",
                             "a=ice-ufrag:" + std::string(257, 'a'),
                             "a=ice-options:", "a=ice-options:a  b", "a=ice-options:google-ice"});
    EXPECT_TRUE(Parsed(with(minimal(), "a=ice-ufrag:" + std::string(256, '+'))).ok());
}

TEST(SdpAttributes, Candidate) {
    EXPECT_EQ(last(Parsed{in_media({"a=candidate:1 1 UDP 2122260223 192.0.2.1 54400 typ host"})}),
              (AttributeValue{Candidate{.foundation = "1",
                                        .component = 1,
                                        .transport = "UDP",
                                        .priority = 2122260223,
                                        .address = "192.0.2.1",
                                        .port = 54400,
                                        .type = "host",
                                        .related_address = {},
                                        .related_port = {},
                                        .extensions = {}}}));
    EXPECT_EQ(last(Parsed{in_media({"a=candidate:2 2 udp 1686052607 203.0.113.9 50000 typ srflx "
                                    "raddr 192.0.2.1 rport 54400 generation 0"})}),
              (AttributeValue{Candidate{.foundation = "2",
                                        .component = 2,
                                        .transport = "udp",
                                        .priority = 1686052607,
                                        .address = "203.0.113.9",
                                        .port = 50000,
                                        .type = "srflx",
                                        .related_address = "192.0.2.1",
                                        .related_port = 54400,
                                        .extensions = "generation 0"}}));
    const Parsed tcp_parsed{in_media(
        {"a=candidate:3 1 tcp 1518280447 x.local 9 typ host tcptype active generation 0"})};
    const auto& tcp = std::get<Candidate>(last(tcp_parsed));
    EXPECT_FALSE(tcp.related_address.has_value());
    EXPECT_EQ(tcp.extensions, "tcptype active generation 0");
    expect_refused_in_media({
        "a=candidate:1 1 udp 1 192.0.2.1 1 host",
        "a=candidate:1 1 udp 0 192.0.2.1 1 typ host",
        "a=candidate:1 1 udp 2147483648 192.0.2.1 1 typ host",
        "a=candidate:1 257 udp 1 192.0.2.1 1 typ host",
        "a=candidate:1 0 udp 1 192.0.2.1 1 typ host",
        "a=candidate:" + std::string(33, 'f') + " 1 udp 1 192.0.2.1 1 typ host",
        "a=candidate:1 1 udp 1 192.0.2.1 1 typ host raddr",
        "a=candidate:1 1 udp 1 192.0.2.1 1 typ host rport x",
        "a=candidate:1 1 udp 1 192.0.2.1 1 typ host ",
        "a=candidate:1 1 udp 1 192.0.2.1 65536 typ host",
    });
    EXPECT_EQ(Parsed(inserted(minimal(), 5, "a=candidate:1 1 udp 1 192.0.2.1 1 typ host")).error(),
              error_at(ErrorCode::MisplacedAttribute, 5));
}

TEST(SdpAttributes, EndOfCandidatesAtEitherLevel) {
    EXPECT_EQ(last(Parsed{in_media({"a=end-of-candidates"})}), (AttributeValue{EndOfCandidates{}}));
    EXPECT_EQ(session_first(Parsed{inserted(minimal(), 5, "a=end-of-candidates")}),
              (AttributeValue{EndOfCandidates{}}));
    expect_refused_in_media({"a=end-of-candidates:", "a=end-of-candidates:1"});
}

TEST(SdpAttributes, Fingerprint) {
    const Parsed p{minimal()};
    const auto& fp = std::get<Fingerprint>(p.session().media[0].attributes[2].value);
    EXPECT_EQ(fp.algorithm, "sha-256");
    EXPECT_TRUE(fp.value.starts_with("05:B8:"));
    // RFC 8122 section 5: hex pairs separated by colons.
    expect_refused_in_media({"a=fingerprint:sha-256 AB:CG", "a=fingerprint:sha-256 ABCD",
                             "a=fingerprint:sha-256 AB:", "a=fingerprint:sha-256 A:BC",
                             "a=fingerprint:sha-256", "a=fingerprint:sha-256 AB CD"});
}

TEST(SdpAttributes, FingerprintHexInEitherCaseIsKeptAsWritten) {
    const auto lowered = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    std::string lower{ulw::test::kFingerprint};
    std::ranges::transform(lower, lower.begin(), lowered);
    std::string mixed{ulw::test::kFingerprint};
    // After "sha-256 ": every second pair in lower case.
    for (std::size_t i = 8; i < mixed.size(); i += 6) {
        mixed[i] = lowered(mixed[i]);
        mixed[i + 1] = lowered(mixed[i + 1]);
    }
    ASSERT_NE(lower, mixed);
    for (const std::string& value : {lower, mixed}) {
        const Parsed p{replaced(minimal(), "a=fingerprint:", "a=fingerprint:" + value)};
        ASSERT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
        const auto& fp = std::get<Fingerprint>(p.session().media[0].attributes[2].value);
        EXPECT_EQ(fp.value, value.substr(8));
        EXPECT_EQ(serialize(p.session()), p.source());
    }
}

TEST(SdpAttributes, Setup) {
    for (const auto& [text_value, role] :
         {std::pair{"active", SetupRole::Active}, std::pair{"passive", SetupRole::Passive},
          std::pair{"actpass", SetupRole::ActPass}, std::pair{"holdconn", SetupRole::HoldConn}}) {
        EXPECT_EQ(last(Parsed{in_media({std::string{"a=setup:"} + text_value})}),
                  (AttributeValue{codec::sdp::Setup{role}}));
    }
    expect_refused_in_media({"a=setup:ACTIVE", "a=setup:", "a=setup"});
}

TEST(SdpAttributes, RtcpMuxAndReducedSizeAreMediaFlags) {
    EXPECT_EQ(last(Parsed{in_media({"a=rtcp-mux"})}), (AttributeValue{RtcpMux{}}));
    EXPECT_EQ(last(Parsed{in_media({"a=rtcp-rsize"})}), (AttributeValue{RtcpRsize{}}));
    expect_refused_in_media({"a=rtcp-mux:1", "a=rtcp-rsize:"});
    EXPECT_EQ(Parsed(inserted(minimal(), 5, "a=rtcp-mux")).error(),
              error_at(ErrorCode::MisplacedAttribute, 5));
}

TEST(SdpAttributes, DirectionsAtEitherLevel) {
    for (const auto& [name, d] :
         {std::pair{"sendrecv", Direction::SendRecv}, std::pair{"sendonly", Direction::SendOnly},
          std::pair{"recvonly", Direction::RecvOnly}, std::pair{"inactive", Direction::Inactive}}) {
        EXPECT_EQ(last(Parsed{in_media({std::string{"a="} + name})}), (AttributeValue{d}));
        EXPECT_EQ(session_first(Parsed{inserted(minimal(), 5, std::string{"a="} + name)}),
                  (AttributeValue{d}));
    }
    expect_refused_in_media({"a=sendrecv:", "a=inactive:x"});
}

TEST(SdpAttributes, Rid) {
    EXPECT_EQ(
        last(Parsed{in_media({"a=rid:q send"})}),
        (AttributeValue{Rid{.id = "q", .direction = RidDirection::Send, .restrictions = {}}}));
    EXPECT_EQ(last(Parsed{in_media({"a=rid:hi_1-x recv pt=96;max-width=1280"})}),
              (AttributeValue{Rid{.id = "hi_1-x",
                                  .direction = RidDirection::Recv,
                                  .restrictions = "pt=96;max-width=1280"}}));
    expect_refused_in_media({"a=rid:q both", "a=rid:q.x send", "a=rid:q", "a=rid:q send "});
}

TEST(SdpAttributes, Simulcast) {
    EXPECT_EQ(last(Parsed{in_media(
                  {"a=rid:q send", "a=rid:h send", "a=rid:f send", "a=simulcast:send q;h;f"})}),
              (AttributeValue{Simulcast{
                  .first = {.direction = RidDirection::Send, .streams = "q;h;f"}, .second = {}}}));
    EXPECT_EQ(last(Parsed{in_media({"a=rid:1 recv", "a=rid:2 recv", "a=rid:3 recv", "a=rid:4 send",
                                    "a=simulcast:recv 1,~2;3 send 4"})}),
              (AttributeValue{Simulcast{
                  .first = {.direction = RidDirection::Recv, .streams = "1,~2;3"},
                  .second = SimulcastStreams{.direction = RidDirection::Send, .streams = "4"}}}));
    expect_refused_in_media({"a=simulcast:send", "a=simulcast:send q recv",
                             "a=simulcast:send q send h", "a=simulcast:send q;;h",
                             "a=simulcast:send ~", "a=simulcast:both q", "a=simulcast:send q,"});
}

} // namespace
