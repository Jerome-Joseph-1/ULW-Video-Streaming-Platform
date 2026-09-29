// M24 acceptance, the parser's half: an offer and an answer captured from Chromium 141 with
// Playwright (tests/e2e/sdp_roundtrip.spec.mjs, ULW_SDP_CAPTURE=1) parse, validate and
// serialize back byte for byte, and read into the model a call would negotiate. The spec then
// has Chromium accept the serialized text.

#include "codec/sdp/serializer.hpp"
#include "codec/sdp/session.hpp"

#include "text.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using codec::sdp::Attribute;
using codec::sdp::Candidate;
using codec::sdp::Direction;
using codec::sdp::find_attribute;
using codec::sdp::Fingerprint;
using codec::sdp::Group;
using codec::sdp::IcePwd;
using codec::sdp::IceUfrag;
using codec::sdp::MediaDescription;
using codec::sdp::Mid;
using codec::sdp::Msid;
using codec::sdp::Rid;
using codec::sdp::RidDirection;
using codec::sdp::RtcpMux;
using codec::sdp::RtcpRsize;
using codec::sdp::Rtpmap;
using codec::sdp::serialize;
using codec::sdp::Session;
using codec::sdp::SetupRole;
using codec::sdp::Simulcast;
using codec::sdp::Ssrc;
using codec::sdp::UnknownAttribute;
using ulw::test::Parsed;

const std::filesystem::path kData{ULW_SDP_DATA_DIR};

// The attribute, or a test failure: gtest reports the exception.
template <typename T> const T& require(std::span<const Attribute> attributes) {
    const T* found = find_attribute<T>(attributes);
    if (found == nullptr) {
        throw std::runtime_error("attribute missing");
    }
    return *found;
}

std::string fixture(std::string_view name) {
    const std::ifstream in{kData / name, std::ios::binary};
    std::ostringstream all;
    all << in.rdbuf();
    return all.str();
}

std::size_t lines_starting(std::string_view text, std::string_view prefix) {
    std::size_t n = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        if (text.substr(at).starts_with(prefix)) {
            ++n;
        }
        at = end == std::string_view::npos ? text.size() : end + 1;
    }
    return n;
}

template <typename T> std::size_t count(const Session& s) {
    std::size_t n = 0;
    const auto in = [&](const std::vector<Attribute>& attributes) {
        n += static_cast<std::size_t>(std::ranges::count_if(
            attributes, [](const Attribute& a) { return std::holds_alternative<T>(a.value); }));
    };
    in(s.attributes);
    for (const MediaDescription& m : s.media) {
        in(m.attributes);
    }
    return n;
}

std::set<std::string_view> unknown_names(const Session& s) {
    std::set<std::string_view> names;
    const auto in = [&](const std::vector<Attribute>& attributes) {
        for (const Attribute& a : attributes) {
            if (const auto* u = std::get_if<UnknownAttribute>(&a.value)) {
                names.insert(u->name);
            }
        }
    };
    in(s.attributes);
    for (const MediaDescription& m : s.media) {
        in(m.attributes);
    }
    return names;
}

// Every attribute Chromium sends that the model types is read as its type: each typed kind
// accounts for all of its lines, and the unknown ones are only those the model leaves alone.
void expect_every_typed_attribute_typed(const Session& s, std::string_view text) {
    EXPECT_EQ(count<Rtpmap>(s), lines_starting(text, "a=rtpmap:"));
    EXPECT_EQ(count<codec::sdp::Fmtp>(s), lines_starting(text, "a=fmtp:"));
    EXPECT_EQ(count<codec::sdp::RtcpFeedback>(s), lines_starting(text, "a=rtcp-fb:"));
    EXPECT_EQ(count<codec::sdp::Extmap>(s), lines_starting(text, "a=extmap:"));
    EXPECT_EQ(count<Candidate>(s), lines_starting(text, "a=candidate:"));
    EXPECT_EQ(count<Ssrc>(s), lines_starting(text, "a=ssrc:"));
    EXPECT_EQ(count<Mid>(s), lines_starting(text, "a=mid:"));
    EXPECT_EQ(count<Direction>(s),
              lines_starting(text, "a=sendrecv") + lines_starting(text, "a=sendonly") +
                  lines_starting(text, "a=recvonly") + lines_starting(text, "a=inactive"));
    const std::set<std::string_view> untyped{"extmap-allow-mixed", "msid-semantic", "rtcp",
                                             "sctp-port", "max-message-size"};
    for (const std::string_view name : unknown_names(s)) {
        EXPECT_TRUE(untyped.contains(name)) << name;
    }
}

void expect_three_bundled_sections(const Session& s) {
    ASSERT_EQ(s.media.size(), 3U);
    EXPECT_EQ(s.media[0].media, "audio");
    EXPECT_EQ(s.media[1].media, "video");
    EXPECT_EQ(s.media[2].media, "application");
    const auto& group = require<Group>(s.attributes);
    EXPECT_EQ(group.semantics, "BUNDLE");
    EXPECT_EQ(group.tags, (std::vector<std::string_view>{"0", "1", "2"}));
    for (std::size_t i = 0; i < s.media.size(); ++i) {
        const auto& a = s.media[i].attributes;
        EXPECT_EQ(require<Mid>(a).tag, std::to_string(i));
        EXPECT_NE(find_attribute<IceUfrag>(a), nullptr);
        EXPECT_NE(find_attribute<IcePwd>(a), nullptr);
        EXPECT_EQ(require<Fingerprint>(a).algorithm, "sha-256");
    }
}

TEST(ChromiumSdp, OfferRoundTripsByteForByte) {
    const std::string text = fixture("chromium_offer.sdp");
    ASSERT_GT(lines_starting(text, "a="), 100U) << "fixture missing or truncated";
    const Parsed p{text};
    ASSERT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
    EXPECT_EQ(serialize(p.session()), text);
}

TEST(ChromiumSdp, OfferReadsAsACallOffer) {
    const std::string text = fixture("chromium_offer.sdp");
    const Parsed p{text};
    ASSERT_TRUE(p.ok());
    const Session& s = p.session();
    expect_three_bundled_sections(s);
    expect_every_typed_attribute_typed(s, text);
    for (const MediaDescription& m : s.media) {
        EXPECT_EQ(require<codec::sdp::Setup>(m.attributes).role, SetupRole::ActPass);
    }

    const auto& audio = s.media[0];
    EXPECT_EQ(std::ranges::count_if(
                  audio.attributes,
                  [](const Attribute& a) { return std::holds_alternative<Rtpmap>(a.value); }),
              static_cast<long>(audio.formats.size()));
    EXPECT_EQ(
        require<Rtpmap>(audio.attributes),
        (Rtpmap{.payload_type = 111, .encoding = "opus", .clock_rate = 48000, .channels = 2}));
    EXPECT_NE(find_attribute<RtcpMux>(audio.attributes), nullptr);
    EXPECT_NE(find_attribute<RtcpRsize>(audio.attributes), nullptr);
    EXPECT_EQ(require<Direction>(audio.attributes), Direction::SendRecv);
    EXPECT_NE(find_attribute<Msid>(audio.attributes), nullptr);
    EXPECT_EQ(require<Ssrc>(audio.attributes).attribute, "cname");

    const auto& video = s.media[1];
    std::vector<std::string_view> rids;
    for (const Attribute& a : video.attributes) {
        if (const auto* r = std::get_if<Rid>(&a.value)) {
            EXPECT_EQ(r->direction, RidDirection::Send);
            rids.push_back(r->id);
        }
    }
    EXPECT_EQ(rids, (std::vector<std::string_view>{"q", "h", "f"}));
    EXPECT_EQ(require<Simulcast>(video.attributes).first.streams, "q;h;f");

    const auto& data = s.media[2];
    EXPECT_EQ(data.protocol, "UDP/DTLS/SCTP");
    EXPECT_EQ(data.formats, (std::vector<std::string_view>{"webrtc-datachannel"}));
    // Taken as soon as one candidate was gathered, so only the first section carries one.
    EXPECT_TRUE(require<Candidate>(audio.attributes).address.ends_with(".local"));
}

TEST(ChromiumSdp, AnswerRoundTripsAndTakesTheActiveRole) {
    const std::string text = fixture("chromium_answer.sdp");
    const Parsed p{text};
    ASSERT_TRUE(p.ok()) << ::testing::PrintToString(p.error());
    EXPECT_EQ(serialize(p.session()), text);
    expect_three_bundled_sections(p.session());
    expect_every_typed_attribute_typed(p.session(), text);
    for (const MediaDescription& m : p.session().media) {
        EXPECT_EQ(require<codec::sdp::Setup>(m.attributes).role, SetupRole::Active);
    }
    // The answering peer had nothing to send.
    EXPECT_EQ(require<Direction>(p.session().media[0].attributes), Direction::RecvOnly);
}

TEST(ChromiumSdp, AnLfOnlyCopyReadsTheSameAndSerializesWithCrlf) {
    const std::string crlf = fixture("chromium_offer.sdp");
    std::string lf;
    std::ranges::copy_if(crlf, std::back_inserter(lf), [](char c) { return c != '\r'; });
    const Parsed a{crlf};
    const Parsed b{lf};
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.session(), b.session());
    EXPECT_EQ(serialize(b.session()), crlf);
}

} // namespace
