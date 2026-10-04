#include "core/util/time.hpp"

#include "access_token.hpp"
#include "support/fake_clock.hpp"
#include "token_reader.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using infra::sfu::livekit::detail::ApiKey;
using infra::sfu::livekit::detail::Grant;
using infra::sfu::livekit::detail::mint_token;
using infra::sfu::livekit::detail::MintedToken;
using infra::sfu::livekit::detail::Permission;
using ulw::test::bool_at;
using ulw::test::read_token;
using ulw::test::ReadToken;
using ulw::test::string_at;

constexpr std::string_view kRoom = "0192f3a4-0000-7000-8000-000000000001";
constexpr std::string_view kIdentity = "alice/0192f3a4-0000-7000-8000-00000000000d";
constexpr core::Seconds kTtl{360};

const ApiKey& test_key() {
    static const ApiKey key{.id = "test-key",
                            .secret = "test-secret-for-unit-tests-only-0123456789"};
    return key;
}

core::WallTime test_now() {
    // Three quarters into a second, so truncation shows.
    return ulw::test::FakeClock{}.wall_now() + std::chrono::milliseconds(750);
}

ReadToken mint_and_read(const Grant& grant) {
    const std::optional<MintedToken> token = mint_token(test_key(), grant, test_now(), kTtl);
    EXPECT_TRUE(token.has_value());
    auto read = read_token(token->jwt, test_key().secret);
    EXPECT_TRUE(read.has_value()) << "signature or encoding broken";
    return std::move(*read);
}

TEST(AccessToken, MatchesAnIndependentlyComputedToken) {
    // From Python's hmac and base64 over the same claims, byte for byte.
    constexpr std::string_view kExpected =
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJpc3MiOiJ0ZXN0LWtleSIsInN1YiI6ImFsaWNlLzAxOTJmM2E0LTAwMDAtNzAwMC04MDAwLTAwMDAwMDAwMDAw"
        "ZCIsIm5iZiI6MTc2NzIyNTYwMCwiZXhwIjoxNzY3MjI1OTYwLCJ2aWRlbyI6eyJyb29tIjoiMDE5MmYzYTQtMDAw"
        "MC03MDAwLTgwMDAtMDAwMDAwMDAwMDAxIiwicm9vbUpvaW4iOnRydWUsImNhblB1Ymxpc2giOnRydWUsImNhblN1"
        "YnNjcmliZSI6dHJ1ZX19."
        "t5Azqc6_y7lZ81o7nk1uv9nsNfICmo11l7LqzVdp9lY";
    const auto token = mint_token(
        test_key(), Grant{.permission = Permission::JoinRoom, .room = kRoom, .identity = kIdentity},
        test_now(), kTtl);
    ASSERT_TRUE(token);
    EXPECT_EQ(token->jwt, kExpected);
}

TEST(AccessToken, IsSignedWithTheSecretAndNoOtherKey) {
    const auto token = mint_token(
        test_key(), Grant{.permission = Permission::JoinRoom, .room = kRoom, .identity = kIdentity},
        test_now(), kTtl);
    ASSERT_TRUE(token);
    EXPECT_TRUE(read_token(token->jwt, test_key().secret));
    EXPECT_FALSE(read_token(token->jwt, "another-secret-of-the-same-length-01234567"));
    const ReadToken read = *read_token(token->jwt, test_key().secret);
    EXPECT_EQ(string_at(read.header, "alg"), "HS256");
    EXPECT_EQ(string_at(read.header, "typ"), "JWT");
}

TEST(AccessToken, ExpiresTtlAfterTheWholeSecondItWasIssuedIn) {
    const auto token = mint_token(
        test_key(), Grant{.permission = Permission::JoinRoom, .room = kRoom, .identity = kIdentity},
        test_now(), kTtl);
    ASSERT_TRUE(token);
    const ReadToken read = *read_token(token->jwt, test_key().secret);
    const auto issued = std::chrono::floor<core::Seconds>(test_now()).time_since_epoch().count();
    ASSERT_NE(read.claims.find("nbf"), nullptr);
    ASSERT_NE(read.claims.find("exp"), nullptr);
    EXPECT_EQ(read.claims.find("nbf")->as_i64(), issued);
    EXPECT_EQ(read.claims.find("exp")->as_i64(), issued + kTtl.count());
    EXPECT_EQ(token->expires_at, core::WallTime{core::Seconds{issued} + kTtl});
    EXPECT_EQ(read.claims.find("iss")->as_string(), "test-key");
}

TEST(AccessToken, JoinGrantAdmitsTheIdentityToOneRoomOnly) {
    const ReadToken read =
        mint_and_read({.permission = Permission::JoinRoom, .room = kRoom, .identity = kIdentity});
    EXPECT_EQ(string_at(read.claims, "sub"), kIdentity);
    EXPECT_EQ(string_at(read.claims, "video", "room"), kRoom);
    EXPECT_EQ(bool_at(read.claims, "video", "roomJoin"), true);
    EXPECT_EQ(bool_at(read.claims, "video", "canPublish"), true);
    EXPECT_EQ(bool_at(read.claims, "video", "canSubscribe"), true);
    EXPECT_EQ(bool_at(read.claims, "video", "roomAdmin"), std::nullopt);
    EXPECT_EQ(bool_at(read.claims, "video", "roomCreate"), std::nullopt);
}

TEST(AccessToken, PublishGrantPublishesCameraAndMicrophoneAndNothingElse) {
    const ReadToken read = mint_and_read(
        {.permission = Permission::PublishToRoom, .room = kRoom, .identity = kIdentity});
    EXPECT_EQ(string_at(read.claims, "sub"), kIdentity);
    EXPECT_EQ(string_at(read.claims, "video", "room"), kRoom);
    EXPECT_EQ(bool_at(read.claims, "video", "roomJoin"), true);
    EXPECT_EQ(bool_at(read.claims, "video", "canPublish"), true);
    // LiveKit reads an absent canSubscribe or canPublishData as allowed, so both are spelled out.
    EXPECT_EQ(bool_at(read.claims, "video", "canSubscribe"), false);
    EXPECT_EQ(bool_at(read.claims, "video", "canPublishData"), false);
    const core::json::Value* sources = read.claims.find("video")->find("canPublishSources");
    ASSERT_NE(sources, nullptr);
    ASSERT_NE(sources->as_array(), nullptr);
    std::vector<std::string_view> names;
    for (const auto& source : *sources->as_array()) {
        names.push_back(source.as_string().value_or(""));
    }
    EXPECT_EQ(names, (std::vector<std::string_view>{"camera", "microphone"}));
    EXPECT_EQ(bool_at(read.claims, "video", "roomCreate"), std::nullopt);
}

TEST(AccessToken, CreateGrantNamesNoRoomAndNoParticipant) {
    const ReadToken read =
        mint_and_read({.permission = Permission::CreateRooms, .room = {}, .identity = {}});
    EXPECT_EQ(bool_at(read.claims, "video", "roomCreate"), true);
    EXPECT_EQ(string_at(read.claims, "video", "room"), std::nullopt);
    EXPECT_EQ(bool_at(read.claims, "video", "roomJoin"), std::nullopt);
    EXPECT_EQ(read.claims.find("sub"), nullptr);
}

TEST(AccessToken, RecordGrantStartsARecorderAndJoinsNothing) {
    const ReadToken read =
        mint_and_read({.permission = Permission::RecordRoom, .room = {}, .identity = {}});
    EXPECT_EQ(bool_at(read.claims, "video", "roomRecord"), true);
    EXPECT_EQ(bool_at(read.claims, "video", "roomCreate"), std::nullopt);
    EXPECT_EQ(bool_at(read.claims, "video", "roomJoin"), std::nullopt);
    EXPECT_EQ(read.claims.find("sub"), nullptr);
}

TEST(AccessToken, AdminGrantReadsOneRoomAndJoinsNothing) {
    const ReadToken read =
        mint_and_read({.permission = Permission::AdminRoom, .room = "r:1", .identity = {}});
    EXPECT_EQ(bool_at(read.claims, "video", "roomAdmin"), true);
    EXPECT_EQ(string_at(read.claims, "video", "room"), "r:1");
    EXPECT_EQ(bool_at(read.claims, "video", "roomCreate"), std::nullopt);
    EXPECT_EQ(bool_at(read.claims, "video", "roomJoin"), std::nullopt);
    EXPECT_EQ(read.claims.find("sub"), nullptr);
}

TEST(AccessToken, ListGrantListsRoomsAndJoinsNothing) {
    const ReadToken read =
        mint_and_read({.permission = Permission::ListRooms, .room = {}, .identity = {}});
    EXPECT_EQ(bool_at(read.claims, "video", "roomList"), true);
    EXPECT_EQ(bool_at(read.claims, "video", "roomCreate"), std::nullopt);
    EXPECT_EQ(bool_at(read.claims, "video", "roomJoin"), std::nullopt);
}

TEST(AccessToken, NamesAreEscapedIntoTheClaims) {
    const ReadToken read =
        mint_and_read({.permission = Permission::JoinRoom, .room = R"(a"b\c)", .identity = "x\ny"});
    EXPECT_EQ(string_at(read.claims, "video", "room"), R"(a"b\c)");
    EXPECT_EQ(string_at(read.claims, "sub"), "x\ny");
}

} // namespace
