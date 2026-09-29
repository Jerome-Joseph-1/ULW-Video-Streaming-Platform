#include "codec/sdp/error.hpp"
#include "codec/sdp/parser.hpp"
#include "codec/sdp/serializer.hpp"
#include "codec/sdp/session.hpp"

#include "text.hpp"

#include <cstddef>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using codec::sdp::Error;
using codec::sdp::ErrorCode;
using codec::sdp::Limits;
using codec::sdp::MediaDescription;
using codec::sdp::Mid;
using codec::sdp::Rtpmap;
using codec::sdp::serialize;
using codec::sdp::UnknownAttribute;
using ulw::test::error_at;
using ulw::test::inserted;
using ulw::test::Lines;
using ulw::test::minimal;
using ulw::test::Parsed;
using ulw::test::replaced;
using ulw::test::text;
using ulw::test::with;

// The session points into the text: a temporary string must not be accepted.
template <typename T>
concept CanParse = requires(T&& text) { codec::sdp::parse(std::forward<T>(text)); };
static_assert(CanParse<std::string&>);
static_assert(CanParse<const std::string&>);
static_assert(CanParse<std::string_view>);
static_assert(CanParse<const char*>);
static_assert(!CanParse<std::string>);
static_assert(!CanParse<const std::string>);

TEST(SdpParser, ReadsTheMinimalDescription) {
    const Parsed p{minimal()};
    ASSERT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
    const auto& s = p.session();
    EXPECT_EQ(s.origin.username, "-");
    EXPECT_EQ(s.origin.session_id, 1U);
    EXPECT_EQ(s.origin.session_version, 2U);
    EXPECT_EQ(s.origin.network_type, "IN");
    EXPECT_EQ(s.origin.address_type, "IP4");
    EXPECT_EQ(s.origin.address, "127.0.0.1");
    EXPECT_EQ(s.name, "-");
    ASSERT_EQ(s.timings.size(), 1U);
    EXPECT_EQ(s.timings[0].start, 0U);
    ASSERT_EQ(s.attributes.size(), 1U);
    ASSERT_EQ(s.media.size(), 1U);
    const MediaDescription& m = s.media[0];
    EXPECT_EQ(m.media, "audio");
    EXPECT_EQ(m.port, 9);
    EXPECT_FALSE(m.port_count.has_value());
    EXPECT_EQ(m.protocol, "UDP/TLS/RTP/SAVPF");
    EXPECT_EQ(m.formats, (std::vector<std::string_view>{"111", "0", "96", "97"}));
    ASSERT_EQ(m.connections.size(), 1U);
    EXPECT_EQ(m.connections[0].address, "0.0.0.0");
    EXPECT_EQ(m.line, 6U);
    ASSERT_EQ(m.attributes.size(), 6U);
    EXPECT_EQ(m.attributes[4].line, 12U);
    EXPECT_EQ(std::get<Mid>(m.attributes[4].value).tag, "0");
    EXPECT_EQ(
        std::get<Rtpmap>(m.attributes[5].value),
        (Rtpmap{.payload_type = 111, .encoding = "opus", .clock_rate = 48000, .channels = 2}));
}

TEST(SdpParser, LfAndCrlfLineEndingsReadTheSame) {
    std::string lf;
    for (const std::string& line : minimal()) {
        lf += line + "\n";
    }
    const Parsed crlf_parsed{minimal()};
    const Parsed lf_parsed{lf};
    ASSERT_TRUE(lf_parsed.ok());
    EXPECT_EQ(lf_parsed.session(), crlf_parsed.session());
    EXPECT_EQ(serialize(lf_parsed.session()), text(minimal()));
}

TEST(SdpParser, TheLastLineNeedNotEndInANewline) {
    std::string t = text(minimal());
    t.resize(t.size() - 2);
    const Parsed p{t};
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(serialize(p.session()), text(minimal()));
}

TEST(SdpParser, RefusesACarriageReturnOrNulInsideALine) {
    EXPECT_EQ(Parsed(replaced(minimal(), "s=", "s=a\rb")).error(),
              error_at(ErrorCode::ForbiddenCharacter, 3));
    EXPECT_EQ(Parsed(replaced(minimal(), "s=", std::string{"s=a\0b", 5})).error(),
              error_at(ErrorCode::ForbiddenCharacter, 3));
    // A CR not followed by LF, at the very end.
    EXPECT_EQ(Parsed(text(minimal()) + "a=x\r").error(),
              error_at(ErrorCode::ForbiddenCharacter, 14));
}

TEST(SdpParser, RefusesLinesThatAreNotTypeEqualsValue) {
    for (const std::string_view bad : {"", "a", "A=b", "a:b", "=a", " a=b", "1=x"}) {
        EXPECT_EQ(Parsed(with(minimal(), bad)).error(), error_at(ErrorCode::MalformedLine, 14))
            << bad;
    }
    // A blank line anywhere, not only at the end.
    EXPECT_EQ(Parsed(inserted(minimal(), 4, "")).error(), error_at(ErrorCode::MalformedLine, 4));
}

TEST(SdpParser, RefusesTypesItDoesNotKnow) {
    // RFC 8866 section 5: a parser must reject a description with a type it does not know.
    EXPECT_EQ(Parsed(inserted(minimal(), 4, "x=1")).error(),
              error_at(ErrorCode::UnknownLineType, 4));
    EXPECT_EQ(Parsed(with(minimal(), "y=1")).error(), error_at(ErrorCode::UnknownLineType, 14));
}

TEST(SdpParser, RequiresVersionOriginNameAndTiming) {
    EXPECT_EQ(Parsed(std::string{}).error(), error_at(ErrorCode::MissingLine, 1));
    EXPECT_EQ(Parsed(replaced(minimal(), "v=", "")).error(), error_at(ErrorCode::MissingLine, 1));
    EXPECT_EQ(Parsed(replaced(minimal(), "o=", "")).error(), error_at(ErrorCode::MissingLine, 2));
    EXPECT_EQ(Parsed(replaced(minimal(), "s=", "")).error(), error_at(ErrorCode::MissingLine, 3));
    // Timing is missing once an attribute arrives where t= should have been.
    EXPECT_EQ(Parsed(replaced(minimal(), "t=", "")).error(), error_at(ErrorCode::MissingLine, 4));
    EXPECT_EQ(Parsed(Lines{"v=0", "o=- 1 2 IN IP4 127.0.0.1", "s=-"}).error(),
              error_at(ErrorCode::MissingLine, 4));
}

TEST(SdpParser, RefusesSessionLinesOutOfOrderOrRepeated) {
    // RFC 8866 section 5: v o s i u e p c b t r z k a, then media.
    EXPECT_EQ(Parsed(inserted(minimal(), 5, "i=late")).error(),
              error_at(ErrorCode::MisplacedLine, 5));
    EXPECT_EQ(Parsed(inserted(minimal(), 3, "c=IN IP4 0.0.0.0")).error(),
              error_at(ErrorCode::MissingLine, 3));
    EXPECT_EQ(Parsed(inserted(minimal(), 4, "s=again")).error(),
              error_at(ErrorCode::MisplacedLine, 4));
    EXPECT_EQ(Parsed(inserted(minimal(), 2, "v=0")).error(), error_at(ErrorCode::MisplacedLine, 2));
    EXPECT_EQ(Parsed(inserted(minimal(), 4, "r=7d 1h 0 25h")).error(),
              error_at(ErrorCode::MisplacedLine, 4));
    EXPECT_EQ(Parsed(inserted(minimal(), 6, "z=2882844526 -1h")).error(),
              error_at(ErrorCode::MisplacedLine, 6));
}

TEST(SdpParser, RefusesMediaLinesOutOfOrderAndSessionLinesInMedia) {
    // m i c b k a within a section.
    EXPECT_EQ(Parsed(with(minimal(), "c=IN IP4 0.0.0.0")).error(),
              error_at(ErrorCode::MisplacedLine, 14));
    EXPECT_EQ(Parsed(inserted(minimal(), 8, "i=after c")).error(),
              error_at(ErrorCode::MisplacedLine, 8));
    EXPECT_EQ(Parsed(with(minimal(), "o=- 1 2 IN IP4 127.0.0.1")).error(),
              error_at(ErrorCode::MisplacedLine, 14));
    EXPECT_EQ(Parsed(with(minimal(), "t=0 0")).error(), error_at(ErrorCode::MisplacedLine, 14));
}

TEST(SdpParser, KeepsEveryOptionalAndRepeatedLineInOrder) {
    const Lines lines{"v=0",
                      "o=jdoe 3724394400 3724394405 IN IP4 198.51.100.1",
                      "s=Call to John Smith",
                      "i=SDP Offer #1",
                      "u=http://www.jdoe.example.com/home.html",
                      "e=Jane Doe <jane@jdoe.example.com>",
                      "e=j@example.com",
                      "p=+1 617 555-6011",
                      "c=IN IP4 198.51.100.1",
                      "b=AS:512",
                      "b=TIAS:500000",
                      "t=0 0",
                      "r=7d 1h 0 25h",
                      "t=3724394400 3724398000",
                      "z=2882844526 -1h 2898848070 0",
                      "k=prompt",
                      "a=recvonly",
                      "a=ice-ufrag:abcd",
                      "a=ice-pwd:aaaaaaaaaaaaaaaaaaaaaa",
                      "a=fingerprint:" + std::string{ulw::test::kFingerprint},
                      "a=setup:actpass",
                      "m=audio 49170/2 RTP/AVP 0",
                      "i=voice",
                      "c=IN IP4 233.252.0.1/127",
                      "c=IN IP4 233.252.0.2/127",
                      "b=AS:64",
                      "k=prompt",
                      "a=mid:a",
                      "m=application 0 UDP/DTLS/SCTP webrtc-datachannel",
                      "a=mid:b"};
    const Parsed p{lines};
    ASSERT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
    const auto& s = p.session();
    EXPECT_EQ(s.emails.size(), 2U);
    EXPECT_EQ(s.phones.size(), 1U);
    EXPECT_EQ(s.bandwidths.size(), 2U);
    EXPECT_EQ(s.bandwidths[1].value, 500000U);
    ASSERT_EQ(s.timings.size(), 2U);
    EXPECT_EQ(s.timings[0].repeats, (std::vector<std::string_view>{"7d 1h 0 25h"}));
    EXPECT_TRUE(s.timings[1].repeats.empty());
    EXPECT_EQ(s.zone_adjustments, "2882844526 -1h 2898848070 0");
    EXPECT_EQ(s.media[0].port_count, 2);
    EXPECT_EQ(s.media[0].connections.size(), 2U);
    EXPECT_EQ(s.media[0].key, "prompt");
    EXPECT_EQ(serialize(s), text(lines));
}

TEST(SdpParser, RefusesMalformedFixedLines) {
    const auto at = [](std::string_view prefix, std::string_view by) {
        return Parsed(replaced(minimal(), prefix, by)).error();
    };
    EXPECT_EQ(at("v=", "v=1"), error_at(ErrorCode::BadVersion, 1));
    EXPECT_EQ(at("o=", "o=- 1 2 IN IP4"), error_at(ErrorCode::BadOrigin, 2));
    EXPECT_EQ(at("o=", "o=- 01 2 IN IP4 127.0.0.1"), error_at(ErrorCode::BadOrigin, 2));
    EXPECT_EQ(at("o=", "o=- 1 2 IN IP4 127.0.0.1 x"), error_at(ErrorCode::BadOrigin, 2));
    EXPECT_EQ(at("o=", "o=-  1 2 IN IP4 127.0.0.1"), error_at(ErrorCode::BadOrigin, 2));
    EXPECT_EQ(at("o=", "o=- 18446744073709551616 2 IN IP4 127.0.0.1"),
              error_at(ErrorCode::BadOrigin, 2));
    EXPECT_EQ(at("s=", "s="), error_at(ErrorCode::BadSessionName, 3));
    EXPECT_EQ(at("t=", "t=0"), error_at(ErrorCode::BadTiming, 4));
    EXPECT_EQ(at("t=", "t=0 0 "), error_at(ErrorCode::BadTiming, 4));
    EXPECT_EQ(at("c=", "c=IN IP4"), error_at(ErrorCode::BadConnection, 7));
    EXPECT_EQ(Parsed(inserted(minimal(), 4, "b=AS:x")).error(),
              error_at(ErrorCode::BadBandwidth, 4));
    EXPECT_EQ(Parsed(inserted(minimal(), 4, "b=AS")).error(), error_at(ErrorCode::BadBandwidth, 4));
    EXPECT_EQ(Parsed(inserted(minimal(), 4, "i=")).error(), error_at(ErrorCode::MalformedLine, 4));
}

TEST(SdpParser, RefusesMalformedMediaLines) {
    const auto media = [](std::string_view m) {
        return Parsed(replaced(minimal(), "m=", m)).error();
    };
    const Error bad = error_at(ErrorCode::BadMediaLine, 6);
    EXPECT_EQ(media("m=audio 9 UDP/TLS/RTP/SAVPF"), bad);
    EXPECT_EQ(media("m=audio 70000 UDP/TLS/RTP/SAVPF 111"), bad);
    EXPECT_EQ(media("m=audio 9/0 UDP/TLS/RTP/SAVPF 111"), bad);
    EXPECT_EQ(media("m=audio 09 UDP/TLS/RTP/SAVPF 111"), bad);
    EXPECT_EQ(media("m=audio 9 UDP//SAVPF 111"), bad);
    // On an RTP protocol, formats are payload types: numbers below 128.
    EXPECT_EQ(media("m=audio 9 UDP/TLS/RTP/SAVPF opus"), bad);
    EXPECT_EQ(media("m=audio 9 UDP/TLS/RTP/SAVPF 128"), bad);
    EXPECT_EQ(media("m=audio 9 UDP/TLS/RTP/SAVPF 111 0 111"),
              error_at(ErrorCode::DuplicateFormat, 6));
}

TEST(SdpParser, EnforcesItsSizeAndCountLimits) {
    const std::string t = text(minimal());
    EXPECT_TRUE(Parsed(t, Limits{.max_bytes = t.size()}).ok());
    EXPECT_EQ(Parsed(t, Limits{.max_bytes = t.size() - 1}).error(),
              error_at(ErrorCode::TooLarge, 0));

    Lines two = with(minimal(), "m=application 0 UDP/DTLS/SCTP webrtc-datachannel");
    two.emplace_back("a=mid:1");
    EXPECT_TRUE(Parsed(text(two), Limits{.max_media_sections = 2}).ok());
    EXPECT_EQ(Parsed(text(two), Limits{.max_media_sections = 1}).error(),
              error_at(ErrorCode::TooManyMediaSections, 14));

    // Six attributes in the section; the session level holds one.
    EXPECT_TRUE(Parsed(t, Limits{.max_attributes = 6}).ok());
    EXPECT_EQ(Parsed(t, Limits{.max_attributes = 5}).error(),
              error_at(ErrorCode::TooManyAttributes, 13));

    std::string formats = "m=application 9 UDP/DTLS/SCTP";
    for (int i = 0; i < 128; ++i) {
        formats += " f" + std::to_string(i);
    }
    EXPECT_TRUE(Parsed(replaced(replaced(minimal(), "m=", formats), "a=rtpmap", "")).ok());
    EXPECT_EQ(Parsed(replaced(minimal(), "m=", formats + " f128")).error(),
              error_at(ErrorCode::TooManyFormats, 6));
}

TEST(SdpParser, KeepsUnknownAttributesVerbatim) {
    const Lines lines = [] {
        Lines l = minimal();
        l.emplace_back("a=x-flag");
        l.emplace_back("a=x-empty:");
        l.emplace_back("a=msid-semantic: WMS  *");
        l.emplace_back("a=RTPMAP:not the typed one");
        return l;
    }();
    const Parsed p{lines};
    ASSERT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
    const auto& a = p.session().media[0].attributes;
    ASSERT_EQ(a.size(), 10U);
    EXPECT_EQ(std::get<UnknownAttribute>(a[6].value),
              (UnknownAttribute{.name = "x-flag", .value = std::nullopt}));
    EXPECT_EQ(std::get<UnknownAttribute>(a[7].value),
              (UnknownAttribute{.name = "x-empty", .value = ""}));
    EXPECT_EQ(std::get<UnknownAttribute>(a[8].value).value, " WMS  *");
    EXPECT_EQ(std::get<UnknownAttribute>(a[9].value).name, "RTPMAP");
    EXPECT_EQ(serialize(p.session()), text(lines));
}

TEST(SdpParser, RefusesAttributeNamesThatAreNotTokens) {
    EXPECT_EQ(Parsed(with(minimal(), "a=")).error(), error_at(ErrorCode::BadAttribute, 14));
    EXPECT_EQ(Parsed(with(minimal(), "a=:x")).error(), error_at(ErrorCode::BadAttribute, 14));
    EXPECT_EQ(Parsed(with(minimal(), "a=a b")).error(), error_at(ErrorCode::BadAttribute, 14));
}

} // namespace
