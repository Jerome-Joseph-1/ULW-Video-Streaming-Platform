// Properties of the parser and serializer together, from fixed seeds so that a failure names a
// reproducible case:
//  - a random valid Session serializes to text that parses back to the same Session;
//  - whatever single line is dropped from or repeated in Chromium's offer, the result is either
//    refused or serializes back to exactly its own text.

#include "codec/sdp/parser.hpp"
#include "codec/sdp/serializer.hpp"
#include "codec/sdp/session.hpp"

#include "text.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using codec::sdp::Attribute;
using codec::sdp::AttributeValue;
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

class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    std::size_t below(std::size_t bound) noexcept { return next() % bound; }
    bool coin() noexcept { return (next() & 1U) != 0; }

private:
    std::uint64_t state_;
};

// Random Sessions that satisfy every rule parse() enforces. The strings live in an arena the
// Session's views point into.
class Generator {
public:
    explicit Generator(std::uint64_t seed) : rng_(seed) {}

    Session session() {
        Session s;
        s.origin = Origin{.username = token(),
                          .session_id = rng_.next(),
                          .session_version = rng_.below(1000),
                          .network_type = "IN",
                          .address_type = rng_.coin() ? "IP4" : "IP6",
                          .address = token()};
        s.name = text();
        if (rng_.coin()) {
            s.information = text();
        }
        for (std::size_t i = rng_.below(3); i > 0; --i) {
            s.emails.push_back(text());
        }
        if (rng_.coin()) {
            s.connection = connection();
        }
        for (std::size_t i = rng_.below(3); i > 0; --i) {
            s.bandwidths.push_back(Bandwidth{.type = token(), .value = rng_.next()});
        }
        for (std::size_t i = 1 + rng_.below(2); i > 0; --i) {
            Timing t{.start = rng_.next(), .stop = rng_.below(10), .repeats = {}};
            for (std::size_t r = rng_.below(3); r > 0; --r) {
                t.repeats.push_back(text());
            }
            s.timings.push_back(std::move(t));
        }
        if (rng_.coin()) {
            s.key = text();
        }

        const std::size_t sections = rng_.below(9);
        // Transport for every section at session level; BUNDLE tags every section.
        Group bundle{.semantics = "BUNDLE", .tags = {}};
        for (std::size_t i = 0; i < sections; ++i) {
            bundle.tags.push_back(keep("m" + std::to_string(i)));
        }
        s.attributes.push_back(Attribute{.value = bundle});
        s.attributes.push_back(Attribute{.value = IceUfrag{ice_chars(4 + rng_.below(8))}});
        s.attributes.push_back(Attribute{.value = IcePwd{ice_chars(22 + rng_.below(8))}});
        s.attributes.push_back(Attribute{
            .value = Fingerprint{.algorithm = "sha-256", .value = kFingerprint.substr(8)}});
        s.attributes.push_back(Attribute{.value = Setup{SetupRole::ActPass}});
        for (std::size_t i = rng_.below(4); i > 0; --i) {
            s.attributes.push_back(Attribute{.value = session_extra()});
        }
        for (std::size_t i = 0; i < sections; ++i) {
            s.media.push_back(media(bundle.tags[i]));
        }
        return s;
    }

private:
    std::string_view keep(std::string s) { return arena_.emplace_back(std::move(s)); }

    std::string_view token() {
        // RFC 8866 token-char, less a few so that output stays readable.
        static constexpr std::string_view kChars =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'*+-.^_`{|}~";
        std::string s;
        for (std::size_t n = 1 + rng_.below(10); n > 0; --n) {
            s += kChars[rng_.below(kChars.size())];
        }
        return keep(std::move(s));
    }

    // Any bytes RFC 8866 allows in a text field: spaces and high bytes included.
    std::string_view text() {
        std::string s;
        for (std::size_t n = 1 + rng_.below(20); n > 0; --n) {
            const auto c = static_cast<char>(0x20 + rng_.below(0xE0));
            s += c == 0x7F ? ' ' : c;
        }
        return keep(std::move(s));
    }

    std::string_view ice_chars(std::size_t n) {
        static constexpr std::string_view kChars =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/";
        std::string s;
        for (; n > 0; --n) {
            s += kChars[rng_.below(kChars.size())];
        }
        return keep(std::move(s));
    }

    std::string_view number(std::uint64_t n) { return keep(std::to_string(n)); }

    Connection connection() {
        return Connection{.network_type = "IN", .address_type = "IP4", .address = token()};
    }

    Direction direction() { return static_cast<Direction>(rng_.below(4)); }

    AttributeValue session_extra() {
        switch (rng_.below(4)) {
        case 0:
            return UnknownAttribute{.name = keep("x-" + std::string{token()}),
                                    .value = rng_.coin() ? std::optional{text()} : std::nullopt};
        case 1:
            return direction();
        case 2:
            return EndOfCandidates{};
        default:
            return IceOptions{.options = {ice_chars(1 + rng_.below(8)), ice_chars(3)}};
        }
    }

    MediaDescription media(std::string_view mid) {
        const bool rtp = rng_.below(4) != 0;
        std::string_view kind = "application";
        if (rtp) {
            kind = rng_.coin() ? "audio" : "video";
        }
        MediaDescription m{.media = kind,
                           .port = static_cast<std::uint16_t>(1 + rng_.below(65535)),
                           .port_count = {},
                           .protocol = rtp ? "UDP/TLS/RTP/SAVPF" : "UDP/DTLS/SCTP",
                           .formats = {},
                           .information = {},
                           .connections = {},
                           .bandwidths = {},
                           .key = {},
                           .attributes = {},
                           .line = 0};
        if (rng_.coin()) {
            m.port_count = static_cast<std::uint16_t>(1 + rng_.below(4));
        }
        if (rng_.coin()) {
            m.information = text();
        }
        for (std::size_t i = rng_.below(3); i > 0; --i) {
            m.connections.push_back(connection());
        }
        m.attributes.push_back(Attribute{.value = Mid{mid}});
        if (!rtp) {
            m.formats.emplace_back("webrtc-datachannel");
            m.attributes.push_back(
                Attribute{.value = UnknownAttribute{.name = "sctp-port", .value = number(5000)}});
            return m;
        }
        std::vector<std::uint8_t> pts;
        const std::size_t first = rng_.below(128);
        for (std::size_t i = 1 + rng_.below(12); i > 0 && pts.size() < 128; --i) {
            pts.push_back(static_cast<std::uint8_t>((first + pts.size()) % 128));
            m.formats.push_back(number(pts.back()));
        }
        std::uint16_t next_extmap = 1;
        std::vector<std::string_view> send_rids;
        for (std::size_t i = rng_.below(30); i > 0; --i) {
            m.attributes.push_back(Attribute{.value = media_extra(pts, next_extmap, send_rids)});
        }
        // Each pt at most once, and after the fact so that the extras above cannot repeat one.
        for (const std::uint8_t pt : pts) {
            if (rng_.coin()) {
                m.attributes.push_back(Attribute{
                    .value = Rtpmap{.payload_type = pt,
                                    .encoding = token(),
                                    .clock_rate = static_cast<std::uint32_t>(1 + rng_.below(96000)),
                                    .channels = {}}});
            }
        }
        if (!send_rids.empty()) {
            std::string list;
            for (const std::string_view id : send_rids) {
                if (!list.empty()) {
                    list += rng_.coin() ? ';' : ',';
                }
                if (rng_.coin()) {
                    list += '~';
                }
                list += id;
            }
            m.attributes.push_back(Attribute{
                .value =
                    Simulcast{.first = {.direction = RidDirection::Send, .streams = keep(list)},
                              .second = {}}});
        }
        return m;
    }

    AttributeValue media_extra(const std::vector<std::uint8_t>& pts, std::uint16_t& next_extmap,
                               std::vector<std::string_view>& send_rids) {
        const std::uint8_t pt = pts[rng_.below(pts.size())];
        switch (rng_.below(12)) {
        case 0:
            return Fmtp{.format = number(pt), .parameters = text()};
        case 1:
            return RtcpFeedback{.payload_type = rng_.coin() ? std::optional{pt} : std::nullopt,
                                .type = token(),
                                .parameter = rng_.coin() ? std::optional{text()} : std::nullopt};
        case 2:
            return Extmap{.id = next_extmap++,
                          .direction = rng_.coin() ? std::optional{direction()} : std::nullopt,
                          .uri = token(),
                          .attributes = rng_.coin() ? std::optional{text()} : std::nullopt};
        case 3:
            return Msid{.stream = token(),
                        .track = rng_.coin() ? std::optional{token()} : std::nullopt};
        case 4:
            return Ssrc{.ssrc = static_cast<std::uint32_t>(rng_.next()),
                        .attribute = token(),
                        .value = rng_.coin() ? std::optional{text()} : std::nullopt};
        case 5:
            return SsrcGroup{.semantics = token(),
                             .ssrcs = {static_cast<std::uint32_t>(rng_.next()), 7}};
        case 6:
            return Candidate{
                .foundation = ice_chars(1 + rng_.below(32)),
                .component = static_cast<std::uint16_t>(1 + rng_.below(256)),
                .transport = rng_.coin() ? "udp" : "tcp",
                .priority = static_cast<std::uint32_t>(1 + rng_.below(0x7FFF'FFFF)),
                .address = token(),
                .port = static_cast<std::uint16_t>(rng_.below(65536)),
                .type = token(),
                .related_address = rng_.coin() ? std::optional{token()} : std::nullopt,
                .related_port = rng_.coin()
                                    ? std::optional{static_cast<std::uint16_t>(rng_.below(65536))}
                                    : std::nullopt,
                // Not starting "raddr" or "rport", which would read as those fields.
                .extensions = rng_.coin() ? std::optional{number(rng_.next())} : std::nullopt};
        case 7: {
            const std::string_view id = keep("r" + std::to_string(send_rids.size()));
            send_rids.push_back(id);
            return Rid{.id = id,
                       .direction = RidDirection::Send,
                       .restrictions = rng_.coin() ? std::optional{text()} : std::nullopt};
        }
        case 8:
            return rng_.coin() ? AttributeValue{RtcpMux{}} : AttributeValue{RtcpRsize{}};
        case 9:
            return direction();
        case 10:
            return Setup{static_cast<SetupRole>(rng_.below(4))};
        default:
            return UnknownAttribute{.name = keep("x-" + std::string{token()}),
                                    .value = rng_.coin() ? std::optional{text()} : std::nullopt};
        }
    }

    SplitMix rng_;
    std::deque<std::string> arena_;
};

TEST(SdpProperty, RandomSessionsParseBackToThemselves) {
    for (std::uint64_t seed = 1; seed <= 1000; ++seed) {
        Generator g{seed};
        const Session built = g.session();
        const std::string text = serialize(built);
        const Parsed p{text};
        ASSERT_TRUE(p.ok()) << "seed " << seed << ": " << ::testing::PrintToString(p.error())
                            << '\n'
                            << text;
        ASSERT_EQ(p.session(), built) << "seed " << seed;
        EXPECT_EQ(serialize(p.session()), text) << "seed " << seed;
    }
}

std::vector<std::string> fixture_lines() {
    std::ifstream in{std::filesystem::path{ULW_SDP_DATA_DIR} / "chromium_offer.sdp"};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// Accepted means reproduced exactly; refused means refused at a line that exists.
void expect_exact_or_refused(const std::vector<std::string>& lines, const std::string& what) {
    const std::string text = ulw::test::text(lines);
    const Parsed p{text};
    if (p.ok()) {
        EXPECT_EQ(serialize(p.session()), text) << what;
    } else {
        EXPECT_LE(p.error()->line, lines.size() + 1) << what;
    }
}

TEST(SdpProperty, DroppingOrRepeatingAnyLineOfTheOfferNeverBreaksTheRoundTrip) {
    const std::vector<std::string> lines = fixture_lines();
    ASSERT_GT(lines.size(), 100U);
    std::size_t refused = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::vector<std::string> dropped = lines;
        dropped.erase(dropped.begin() + static_cast<long>(i));
        expect_exact_or_refused(dropped, "without line " + std::to_string(i + 1));
        refused += Parsed(ulw::test::text(dropped)).ok() ? 0U : 1U;

        std::vector<std::string> repeated = lines;
        repeated.insert(repeated.begin() + static_cast<long>(i), lines[i]);
        expect_exact_or_refused(repeated, "with line " + std::to_string(i + 1) + " twice");
    }
    // v, o, s, t, the group, the m= and mid lines and the like are load-bearing; most
    // attributes are not. A parser that refused everything, or nothing, fails here.
    EXPECT_GT(refused, 5U);
    EXPECT_LT(refused, lines.size() / 2);
}

} // namespace
