// What the signalling boundary enforces on a description that parses: one rule per test, each
// broken once against minimal() and pinned to the line at fault.

#include "codec/sdp/error.hpp"

#include "text.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using codec::sdp::ErrorCode;
using ulw::test::error_at;
using ulw::test::inserted;
using ulw::test::kFingerprint;
using ulw::test::Lines;
using ulw::test::minimal;
using ulw::test::Parsed;
using ulw::test::replaced;
using ulw::test::with;

// minimal() plus a second, video section with mid 1 (lines 14 to 21), inside the BUNDLE group.
Lines two_sections() {
    Lines l = replaced(minimal(), "a=group:", "a=group:BUNDLE 0 1");
    l.insert(l.end(),
             {"m=video 9 UDP/TLS/RTP/SAVPF 96", "c=IN IP4 0.0.0.0", "a=ice-ufrag:efgh",
              "a=ice-pwd:bbbbbbbbbbbbbbbbbbbbbb", "a=fingerprint:" + std::string{kFingerprint},
              "a=setup:actpass", "a=mid:1", "a=rtpmap:96 VP8/90000"});
    return l;
}

TEST(SdpValidate, TheBaseDescriptionsAreValid) {
    EXPECT_TRUE(Parsed(minimal()).ok());
    EXPECT_TRUE(Parsed(two_sections()).ok());
}

TEST(SdpValidate, EverySectionHasExactlyOneMid) {
    EXPECT_EQ(Parsed(replaced(minimal(), "a=mid:", "")).error(),
              error_at(ErrorCode::MissingMid, 6));
    EXPECT_EQ(Parsed(with(minimal(), "a=mid:again")).error(),
              error_at(ErrorCode::DuplicateMid, 14));
}

TEST(SdpValidate, MidsAreUniqueAcrossSections) {
    Lines l = two_sections();
    l[19] = "a=mid:0";
    EXPECT_EQ(Parsed(l).error(), error_at(ErrorCode::DuplicateMid, 20));
}

TEST(SdpValidate, BundleGroupsNameExistingSectionsOnce) {
    EXPECT_EQ(Parsed(replaced(minimal(), "a=group:", "a=group:BUNDLE 0 9")).error(),
              error_at(ErrorCode::BadBundleGroup, 5));
    EXPECT_EQ(Parsed(replaced(minimal(), "a=group:", "a=group:BUNDLE 0 0")).error(),
              error_at(ErrorCode::BadBundleGroup, 5));
    EXPECT_EQ(Parsed(inserted(two_sections(), 6, "a=group:BUNDLE 1")).error(),
              error_at(ErrorCode::BadBundleGroup, 6));
    // Groups of other semantics are not BUNDLE's business.
    EXPECT_TRUE(Parsed(inserted(two_sections(), 6, "a=group:LS 0 1")).ok());
}

TEST(SdpValidate, TheTaggedSectionOfABundleIsNotRejectedOrBundleOnly) {
    Lines rejected = two_sections();
    rejected[5] = "m=audio 0 UDP/TLS/RTP/SAVPF 111 0 96 97";
    EXPECT_EQ(Parsed(rejected).error(), error_at(ErrorCode::BadBundleGroup, 5));
    EXPECT_EQ(Parsed(with(minimal(), "a=bundle-only")).error(),
              error_at(ErrorCode::BadBundleGroup, 5));
    // The same section is fine second in the group, where RFC 9143 puts bundle-only ones.
    Lines second = replaced(two_sections(), "a=group:", "a=group:BUNDLE 1 0");
    second[5] = "m=audio 0 UDP/TLS/RTP/SAVPF 111 0 96 97";
    second = inserted(second, 14, "a=bundle-only");
    EXPECT_TRUE(Parsed(second).ok()) << ::testing::PrintToString(Parsed(second).error());
}

TEST(SdpValidate, BundleOnlySectionsMustBeInAGroup) {
    const Lines l = replaced(with(minimal(), "a=bundle-only"), "a=group:", "");
    EXPECT_EQ(Parsed(l).error(), error_at(ErrorCode::BadBundleGroup, 5));
}

TEST(SdpValidate, TransportIsDescribedWhereTheSectionNeedsIt) {
    EXPECT_EQ(Parsed(replaced(minimal(), "a=ice-ufrag:", "")).error(),
              error_at(ErrorCode::MissingIceCredentials, 6));
    EXPECT_EQ(Parsed(replaced(minimal(), "a=ice-pwd:", "")).error(),
              error_at(ErrorCode::MissingIceCredentials, 6));
    EXPECT_EQ(Parsed(replaced(minimal(), "a=fingerprint:", "")).error(),
              error_at(ErrorCode::MissingFingerprint, 6));
    EXPECT_EQ(Parsed(replaced(minimal(), "a=setup:", "")).error(),
              error_at(ErrorCode::MissingSetup, 6));
    // A section outside any bundle describes its own transport.
    EXPECT_EQ(Parsed(replaced(replaced(minimal(), "a=group:", ""), "a=setup:", "")).error(),
              error_at(ErrorCode::MissingSetup, 5));
}

TEST(SdpValidate, SessionLevelTransportCoversEverySection) {
    Lines l = minimal();
    for (const std::string_view prefix :
         {"a=ice-ufrag:", "a=ice-pwd:", "a=fingerprint:", "a=setup:"}) {
        const auto it = std::ranges::find_if(
            l, [&](const std::string& line) { return line.starts_with(prefix); });
        const std::string line = std::move(*it);
        l.erase(it);
        l.insert(l.begin() + 4, line);
    }
    EXPECT_TRUE(Parsed(l).ok()) << ::testing::PrintToString(Parsed(l).error());
}

TEST(SdpValidate, BundledSectionsTakeTheTaggedSectionsTransport) {
    Lines l = two_sections();
    l.erase(l.begin() + 15, l.begin() + 19);
    EXPECT_TRUE(Parsed(l).ok()) << ::testing::PrintToString(Parsed(l).error());
    // Without the group, the second section needs its own.
    EXPECT_EQ(Parsed(replaced(l, "a=group:", "")).error(),
              error_at(ErrorCode::MissingIceCredentials, 13));
}

TEST(SdpValidate, RejectedSectionsOutsideABundleNeedNoTransport) {
    Lines l = two_sections();
    l.erase(l.begin() + 15, l.begin() + 19);
    l = replaced(l, "a=group:", "a=group:BUNDLE 0");
    l[13] = "m=video 0 UDP/TLS/RTP/SAVPF 96";
    EXPECT_TRUE(Parsed(l).ok()) << ::testing::PrintToString(Parsed(l).error());
}

TEST(SdpValidate, FingerprintAlgorithmsAreAllowlisted) {
    const auto fp = [](std::string_view value) {
        return Parsed(replaced(minimal(), "a=fingerprint:",
                               std::string{"a=fingerprint:"} + std::string{value}))
            .error();
    };
    const std::string twenty_bytes = "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67";
    EXPECT_EQ(fp("sha-1 " + twenty_bytes),
              error_at(ErrorCode::UnsupportedFingerprintAlgorithm, 10));
    EXPECT_EQ(fp("md5 01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF"),
              error_at(ErrorCode::UnsupportedFingerprintAlgorithm, 10));
    // The hash function token is case-insensitive (RFC 8122 section 5).
    EXPECT_EQ(fp("SHA-256" + std::string{kFingerprint.substr(7)}), std::nullopt);
    EXPECT_EQ(fp("sha-256 " + twenty_bytes), error_at(ErrorCode::BadFingerprint, 10));
    std::string sixty_four_bytes = "00";
    for (int i = 1; i < 64; ++i) {
        sixty_four_bytes += ":00";
    }
    EXPECT_EQ(fp("sha-512 " + sixty_four_bytes), std::nullopt);
    EXPECT_EQ(fp("sha-384 " + sixty_four_bytes), error_at(ErrorCode::BadFingerprint, 10));
    // Checked at session level too.
    EXPECT_EQ(Parsed(inserted(minimal(), 5, "a=fingerprint:sha-1 " + twenty_bytes)).error(),
              error_at(ErrorCode::UnsupportedFingerprintAlgorithm, 5));
}

TEST(SdpValidate, PayloadTypesAreThoseOfTheMediaLineAndMappedOnce) {
    EXPECT_EQ(Parsed(with(minimal(), "a=rtpmap:98 VP8/90000")).error(),
              error_at(ErrorCode::UnknownPayloadType, 14));
    EXPECT_EQ(Parsed(with(minimal(), "a=rtpmap:111 opus/48000/2")).error(),
              error_at(ErrorCode::DuplicatePayloadType, 14));
    EXPECT_EQ(Parsed(with(minimal(), "a=fmtp:98 apt=96")).error(),
              error_at(ErrorCode::UnknownPayloadType, 14));
    EXPECT_EQ(Parsed(with(minimal(), "a=rtcp-fb:98 nack")).error(),
              error_at(ErrorCode::UnknownPayloadType, 14));
    EXPECT_TRUE(Parsed(with(minimal(), "a=rtcp-fb:* nack")).ok());
    // A data channel section has no payload types to map.
    const Lines data =
        replaced(minimal(), "m=", "m=application 9 UDP/DTLS/SCTP webrtc-datachannel");
    EXPECT_EQ(Parsed(data).error(), error_at(ErrorCode::UnknownPayloadType, 13));
    EXPECT_TRUE(
        Parsed(replaced(data, "a=rtpmap", "a=fmtp:webrtc-datachannel max-message-size=1")).ok());
}

TEST(SdpValidate, ExtmapIdsAreUniqueAcrossSessionAndSection) {
    EXPECT_TRUE(Parsed(with(with(minimal(), "a=extmap:1 urn:a"), "a=extmap:2 urn:b")).ok());
    EXPECT_EQ(Parsed(with(with(minimal(), "a=extmap:1 urn:a"), "a=extmap:1 urn:b")).error(),
              error_at(ErrorCode::DuplicateExtmapId, 15));
    EXPECT_EQ(Parsed(with(inserted(minimal(), 5, "a=extmap:1 urn:a"), "a=extmap:1 urn:b")).error(),
              error_at(ErrorCode::DuplicateExtmapId, 15));
    EXPECT_EQ(
        Parsed(inserted(inserted(minimal(), 5, "a=extmap:1 urn:a"), 5, "a=extmap:1 urn:b")).error(),
        error_at(ErrorCode::DuplicateExtmapId, 6));
}

TEST(SdpValidate, SimulcastNamesDeclaredRidsOfItsDirection) {
    EXPECT_TRUE(Parsed(with(with(minimal(), "a=rid:q send"), "a=simulcast:send ~q")).ok());
    EXPECT_EQ(Parsed(with(with(minimal(), "a=rid:q send"), "a=simulcast:send q;h")).error(),
              error_at(ErrorCode::UnknownRid, 15));
    EXPECT_EQ(Parsed(with(with(minimal(), "a=rid:q recv"), "a=simulcast:send q")).error(),
              error_at(ErrorCode::UnknownRid, 15));
    EXPECT_EQ(Parsed(with(with(minimal(), "a=rid:q send"), "a=simulcast:send q recv r")).error(),
              error_at(ErrorCode::UnknownRid, 15));
}

} // namespace
