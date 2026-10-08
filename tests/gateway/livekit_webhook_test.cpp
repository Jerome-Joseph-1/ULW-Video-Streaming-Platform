#include "core/models/ids.hpp"

#include "livekit_webhook.hpp"
#include "webhook_signer.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <string>

namespace {

using gateway::parse_webhook_event;
using gateway::publisher_of;
using gateway::stream_of_room;
using gateway::verify_webhook;
using gateway::WebhookEventKind;
using gateway::WebhookKey;
using gateway::WebhookRejection;
using ulw::test::kWebhookKey;
using ulw::test::kWebhookSecret;
using ulw::test::livekit_token;
using ulw::test::sign_webhook;

constexpr std::int64_t kNow = 1'767'225'600;
constexpr std::string_view kStream = "0192f3a4-0000-7000-8000-0000000000aa";

// A participant_joined as LiveKit v1.13.7 posts it: protojson, lowerCamelCase, int64 as text.
const std::string kJoined = R"({"event":"participant_joined","room":{"sid":"RM_abc","name":")" +
                            std::string(kStream) +
                            R"(:1","emptyTimeout":60,"creationTime":"1767225590"},)"
                            R"("participant":{"sid":"PA_one","identity":"auth0|caster/)" +
                            std::string(kStream) +
                            R"(","state":"ACTIVE","joinedAt":"1767225600"},)"
                            R"("id":"EV_1","createdAt":"1767225600"})";

const WebhookKey kKey{.id = std::string(kWebhookKey), .secret = std::string(kWebhookSecret)};

core::WallTime at(std::int64_t seconds) {
    return core::WallTime{std::chrono::seconds(seconds)};
}

TEST(WebhookVerification, AcceptsWhatLiveKitSigns) {
    EXPECT_TRUE(verify_webhook(livekit_token(kJoined, kNow), kJoined, kKey, at(kNow)));
    // Anywhere in its five minutes, and within the leeway either side.
    EXPECT_TRUE(verify_webhook(livekit_token(kJoined, kNow), kJoined, kKey, at(kNow + 300 + 60)));
    EXPECT_TRUE(verify_webhook(livekit_token(kJoined, kNow), kJoined, kKey, at(kNow - 60)));
}

TEST(WebhookVerification, RefusesAnotherSecret) {
    const std::string token =
        sign_webhook(kJoined, {.secret = "fake-other-secret-0123456789abcdef0123",
                               .issued = kNow,
                               .expires = kNow + 300,
                               .not_before = kNow});
    EXPECT_EQ(verify_webhook(token, kJoined, kKey, at(kNow)),
              std::unexpected(WebhookRejection::Signature));
}

TEST(WebhookVerification, RefusesAnotherKeysToken) {
    const std::string token =
        sign_webhook(kJoined, {.key = "fake-other-key", .issued = kNow, .expires = kNow + 300});
    EXPECT_EQ(verify_webhook(token, kJoined, kKey, at(kNow)),
              std::unexpected(WebhookRejection::UnknownKey));
}

TEST(WebhookVerification, RefusesABodyChangedOnTheWay) {
    const std::string token = livekit_token(kJoined, kNow);
    std::string tampered = kJoined;
    tampered.replace(tampered.find("participant_joined"), 18, "participant_left__");
    EXPECT_EQ(verify_webhook(token, tampered, kKey, at(kNow)),
              std::unexpected(WebhookRejection::BodyHash));
    EXPECT_EQ(verify_webhook(token, kJoined + " ", kKey, at(kNow)),
              std::unexpected(WebhookRejection::BodyHash));
    EXPECT_EQ(verify_webhook(token, "", kKey, at(kNow)),
              std::unexpected(WebhookRejection::BodyHash));
}

TEST(WebhookVerification, RefusesATokenWithoutTheBodysHash) {
    const std::string token =
        sign_webhook(kJoined, {.issued = kNow, .expires = kNow + 300, .sha256 = ""});
    EXPECT_EQ(verify_webhook(token, kJoined, kKey, at(kNow)),
              std::unexpected(WebhookRejection::BodyHash));
}

TEST(WebhookVerification, RefusesATokenChangedOnTheWay) {
    // Another body's hash put in the claims after signing: the signature no longer holds.
    const std::string token = livekit_token(kJoined, kNow);
    const std::string other = livekit_token("{}", kNow);
    const auto claims = [](const std::string& t) {
        const auto a = t.find('.');
        return t.substr(a + 1, t.find('.', a + 1) - a - 1);
    };
    std::string spliced = token;
    spliced.replace(token.find('.') + 1, claims(token).size(), claims(other));
    EXPECT_EQ(verify_webhook(spliced, "{}", kKey, at(kNow)),
              std::unexpected(WebhookRejection::Signature));
}

TEST(WebhookVerification, RefusesAnExpiredToken) {
    EXPECT_EQ(verify_webhook(livekit_token(kJoined, kNow), kJoined, kKey, at(kNow + 300 + 61)),
              std::unexpected(WebhookRejection::Expired));
    // A replay a day later.
    EXPECT_EQ(verify_webhook(livekit_token(kJoined, kNow), kJoined, kKey, at(kNow + 86'400)),
              std::unexpected(WebhookRejection::Expired));
}

TEST(WebhookVerification, RefusesATokenWithoutExpiry) {
    const std::string token = sign_webhook(kJoined, {.issued = kNow});
    EXPECT_EQ(verify_webhook(token, kJoined, kKey, at(kNow)),
              std::unexpected(WebhookRejection::NoExpiry));
}

TEST(WebhookVerification, RefusesATokenFromTheFuture) {
    EXPECT_EQ(verify_webhook(livekit_token(kJoined, kNow), kJoined, kKey, at(kNow - 61)),
              std::unexpected(WebhookRejection::NotYetValid));
}

TEST(WebhookVerification, RefusesAMissingHeader) {
    EXPECT_EQ(verify_webhook("", kJoined, kKey, at(kNow)),
              std::unexpected(WebhookRejection::NoAuthorization));
}

TEST(WebhookVerification, RefusesAnOversizedBody) {
    const std::string body(gateway::kMaxWebhookBody + 1, ' ');
    EXPECT_EQ(verify_webhook(livekit_token(body, kNow), body, kKey, at(kNow)),
              std::unexpected(WebhookRejection::TooLarge));
    const std::string largest(gateway::kMaxWebhookBody, ' ');
    EXPECT_TRUE(verify_webhook(livekit_token(largest, kNow), largest, kKey, at(kNow)));
}

TEST(WebhookVerification, RefusesAnythingButHs256) {
    for (const std::string_view alg : {"none", "HS512", "RS256", "hs256"}) {
        const std::string token =
            sign_webhook(kJoined, {.alg = std::string(alg), .issued = kNow, .expires = kNow + 300});
        EXPECT_EQ(verify_webhook(token, kJoined, kKey, at(kNow)),
                  std::unexpected(WebhookRejection::Algorithm))
            << alg;
    }
}

TEST(WebhookVerification, RefusesWhatIsNoToken) {
    const std::string good = livekit_token(kJoined, kNow);
    const std::string sig = good.substr(good.rfind('.') + 1);
    for (const std::string& bad :
         {std::string("Bearer ") + good, std::string("a.b"), std::string("a.b.c.d"),
          std::string("..") + sig, good + ".", std::string("!!!.e30.") + sig,
          std::string(gateway::kMaxWebhookToken + 1, 'a')}) {
        const auto r = verify_webhook(bad, kJoined, kKey, at(kNow));
        ASSERT_FALSE(r) << bad;
        EXPECT_TRUE(r.error() == WebhookRejection::Malformed ||
                    r.error() == WebhookRejection::Signature)
            << bad << ": " << gateway::to_string(r.error());
    }
    // The bearer scheme LiveKit never sends is not taken off: the header is the token.
    EXPECT_EQ(verify_webhook("Bearer " + good, kJoined, kKey, at(kNow)),
              std::unexpected(WebhookRejection::Malformed));
}

TEST(WebhookVerification, RefusesClaimsThatAreNotLiveKits) {
    const std::string hash = ulw::test::body_sha256(kJoined);
    const auto with = [&](std::string claims) {
        return verify_webhook(sign_webhook(kJoined, {.claims = std::move(claims)}), kJoined, kKey,
                              at(kNow));
    };
    EXPECT_TRUE(with(R"({"iss":"fake-webhook-key","exp":1767225900,"sha256":")" + hash + "\"}"));
    EXPECT_EQ(with(R"({"iss":"fake-webhook-key","exp":"soon","sha256":")" + hash + "\"}"),
              std::unexpected(WebhookRejection::Malformed));
    EXPECT_EQ(with(R"({"iss":"fake-webhook-key","exp":1767225900.5,"sha256":")" + hash + "\"}"),
              std::unexpected(WebhookRejection::Malformed));
    EXPECT_EQ(with(R"({"exp":1767225900,"sha256":")" + hash + "\"}"),
              std::unexpected(WebhookRejection::UnknownKey));
    EXPECT_EQ(with(R"({"iss":"fake-webhook-key","exp":1767225900,"sha256":7})"),
              std::unexpected(WebhookRejection::BodyHash));
    EXPECT_EQ(with("[]"), std::unexpected(WebhookRejection::Malformed));
    // A second exp that a lenient reader might take instead of the first.
    EXPECT_EQ(
        with(R"({"iss":"fake-webhook-key","exp":1,"exp":1767225900,"sha256":")" + hash + "\"}"),
        std::unexpected(WebhookRejection::Malformed));
}

TEST(WebhookEvent, ReadsTheRoomAndTheParticipant) {
    const auto e = parse_webhook_event(kJoined);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->kind, WebhookEventKind::ParticipantJoined);
    EXPECT_EQ(e->id, "EV_1");
    EXPECT_EQ(e->room, std::string(kStream) + ":1");
    EXPECT_EQ(e->identity, "auth0|caster/" + std::string(kStream));
    EXPECT_EQ(e->participant_sid, "PA_one");
}

TEST(WebhookEvent, NamesTheEventsActedOnAndNoOthers) {
    const auto kind = [](std::string_view name) {
        const auto e = parse_webhook_event(R"({"event":")" + std::string(name) + R"("})");
        return e ? e->kind : WebhookEventKind::Other;
    };
    EXPECT_EQ(kind("participant_joined"), WebhookEventKind::ParticipantJoined);
    EXPECT_EQ(kind("track_published"), WebhookEventKind::TrackPublished);
    EXPECT_EQ(kind("participant_left"), WebhookEventKind::ParticipantLeft);
    EXPECT_EQ(kind("participant_connection_aborted"), WebhookEventKind::ParticipantAborted);
    EXPECT_EQ(kind("room_finished"), WebhookEventKind::RoomFinished);
    for (const std::string_view other :
         {"room_started", "track_unpublished", "egress_started", "egress_ended", "", "x"}) {
        EXPECT_EQ(kind(other), WebhookEventKind::Other) << other;
    }
}

TEST(WebhookEvent, RefusesWhatIsNoEvent) {
    EXPECT_FALSE(parse_webhook_event(""));
    EXPECT_FALSE(parse_webhook_event("[]"));
    EXPECT_FALSE(parse_webhook_event(R"({"room":{}})"));
    EXPECT_FALSE(parse_webhook_event(R"({"event":1})"));
    EXPECT_FALSE(parse_webhook_event(R"({"event":"a","event":"b"})"));
    // Fields of the wrong kind are left empty, not trusted.
    const auto e = parse_webhook_event(R"({"event":"participant_left","room":"x"})");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->room, "");
}

TEST(WebhookEvent, OnlyAStreamsOwnRoomNamesIt) {
    const auto stream = stream_of_room(std::string(kStream) + ":1");
    ASSERT_TRUE(stream);
    EXPECT_EQ(stream->to_string(), kStream);
    EXPECT_FALSE(stream_of_room(kStream));
    EXPECT_FALSE(stream_of_room(std::string(kStream) + ":2"));
    EXPECT_FALSE(stream_of_room(std::string(kStream) + ":11"));
    EXPECT_FALSE(stream_of_room("not-a-stream:1"));
    EXPECT_FALSE(stream_of_room(":1"));
    EXPECT_FALSE(stream_of_room(""));
}

TEST(WebhookEvent, OnlyTheStreamsPublisherIdentityNamesItsOwner) {
    const auto stream = *core::LiveStreamId::parse(kStream);
    const auto owner = publisher_of("auth0|caster/" + std::string(kStream), stream);
    ASSERT_TRUE(owner);
    EXPECT_EQ(owner->view(), "auth0|caster");
    // A call member, another stream's publisher, LiveKit's recorder, and nonsense.
    EXPECT_FALSE(publisher_of("auth0|caster/0192f3a4-0000-7000-8000-0000000000bb", stream));
    EXPECT_FALSE(publisher_of("EG_abcdef", stream));
    EXPECT_FALSE(publisher_of("/" + std::string(kStream), stream));
    EXPECT_FALSE(publisher_of("a/b/" + std::string(kStream), stream));
    EXPECT_FALSE(publisher_of(std::string(kStream), stream));
}

} // namespace
