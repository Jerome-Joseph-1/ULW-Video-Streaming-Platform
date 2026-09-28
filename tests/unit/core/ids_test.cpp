#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
#include "core/util/uuid.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"

#include <array>
#include <concepts>
#include <expected>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace {

using core::DeviceId;
using core::DomainError;
using core::NodeId;
using core::RoomId;
using core::UploadId;
using core::UserId;
using core::VideoId;
using ulw::test::FakeClock;
using ulw::test::FakeRandom;

constexpr std::string_view kSample = "0192f3c4-7a1b-7c2d-8e3f-0123456789ab";

static_assert(!std::is_convertible_v<VideoId, UploadId>);
static_assert(!std::is_constructible_v<VideoId, UploadId>);
static_assert(!std::is_constructible_v<RoomId, DeviceId>);
static_assert(!std::is_constructible_v<VideoId, core::Uuid>);
static_assert(!std::equality_comparable_with<VideoId, UploadId>);
static_assert(!std::is_default_constructible_v<VideoId>);
static_assert(!std::is_default_constructible_v<UserId>);
static_assert(!std::is_default_constructible_v<NodeId>);

TEST(UuidId, ParsesTheCanonicalForm) {
    const auto id = VideoId::parse(kSample);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->to_string(), kSample);
    std::array<char, core::Uuid::kTextLength> buffer{};
    id->format_to(buffer);
    EXPECT_EQ(std::string_view(buffer.data(), buffer.size()), kSample);
}

TEST(UuidId, DistinguishesEmptyFromMalformed) {
    EXPECT_EQ(VideoId::parse(""), std::unexpected(DomainError::EmptyIdentifier));
    EXPECT_EQ(VideoId::parse("0192F3C4-7A1B-7C2D-8E3F-0123456789AB"),
              std::unexpected(DomainError::MalformedIdentifier));
    EXPECT_EQ(VideoId::parse("not-a-uuid"), std::unexpected(DomainError::MalformedIdentifier));
}

TEST(UuidId, GeneratesTimeOrderedV7Ids) {
    FakeClock clock;
    FakeRandom random;
    const UploadId first = UploadId::generate(clock, random);
    clock.advance(core::Millis{1});
    const UploadId second = UploadId::generate(clock, random);
    // FakeClock starts at 2026-01-01T00:00:00Z.
    EXPECT_TRUE(first.to_string().starts_with("019b76da-a800-7")) << first.to_string();
    EXPECT_LT(first, second);
}

TEST(UuidId, KeysHashContainers) {
    const FakeClock clock;
    FakeRandom random;
    const RoomId a = RoomId::generate(clock, random);
    const RoomId b = RoomId::generate(clock, random);
    const auto a_again = RoomId::parse(a.to_string());
    ASSERT_TRUE(a_again.has_value());
    EXPECT_EQ(std::hash<RoomId>{}(*a_again), std::hash<RoomId>{}(a));
    const std::unordered_set<RoomId> rooms{a, b, *a_again};
    EXPECT_EQ(rooms.size(), 2U);
}

TEST(UserId, AcceptsSubjectsIssuedByCommonProviders) {
    for (const std::string_view sub : {
             "auth0|5f7c8ec7c33c6c004bbafe82",
             "google-oauth2|103547991597142817347",
             "00u1a2b3c4d5e6f7g8h9",
             "AAAAAAAAAAAAAAAAAAAAAIkzqFVrSaSaFHy782bbtaQ",
             "001234.0123456789abcdef0123456789abcdef.1234",
             "samlp|contoso|jane.doe+video@contoso.com",
             "urn:example:user:42",
         }) {
        const auto id = UserId::parse(sub);
        ASSERT_TRUE(id.has_value()) << sub;
        EXPECT_EQ(id->view(), sub);
    }
}

TEST(UserId, EnforcesTheLengthBoundExactly) {
    const auto longest = UserId::parse(std::string(UserId::kMaxLength, 'u'));
    ASSERT_TRUE(longest.has_value());
    EXPECT_EQ(longest->view().size(), UserId::kMaxLength);
    EXPECT_EQ(UserId::parse(std::string(UserId::kMaxLength + 1, 'u')),
              std::unexpected(DomainError::MalformedIdentifier));
    EXPECT_EQ(UserId::parse(""), std::unexpected(DomainError::EmptyIdentifier));
}

TEST(UserId, RejectsCharactersOutsideTheSubjectAlphabet) {
    for (const std::string_view sub :
         {std::string_view("user 1"), std::string_view("user/1"), std::string_view("user\"1"),
          std::string_view("user\n1"), std::string_view("user%201"), std::string_view("user#1"),
          std::string_view("us\xc3\xa9r"), std::string_view("user\0001", 6)}) {
        EXPECT_EQ(UserId::parse(sub), std::unexpected(DomainError::MalformedIdentifier)) << sub;
    }
}

TEST(UserId, ComparesAndHashesByValue) {
    const auto a = UserId::parse("auth0|abc");
    const auto same = UserId::parse("auth0|abc");
    const auto longer = UserId::parse("auth0|abcd");
    ASSERT_TRUE(a.has_value() && same.has_value() && longer.has_value());
    EXPECT_EQ(*a, *same);
    EXPECT_NE(*a, *longer);
    EXPECT_EQ(std::hash<UserId>{}(*a), std::hash<UserId>{}(*same));
}

TEST(NodeId, AcceptsPodHostnames) {
    for (const std::string_view name : {"gateway-7d9f8b6c5-x2x4z", "worker-0", "a", "0"}) {
        const auto id = NodeId::parse(name);
        ASSERT_TRUE(id.has_value()) << name;
        EXPECT_EQ(id->view(), name);
    }
}

TEST(NodeId, EnforcesTheLabelLengthBoundExactly) {
    EXPECT_TRUE(NodeId::parse(std::string(NodeId::kMaxLength, 'n')).has_value());
    EXPECT_EQ(NodeId::parse(std::string(NodeId::kMaxLength + 1, 'n')),
              std::unexpected(DomainError::MalformedIdentifier));
    EXPECT_EQ(NodeId::parse(""), std::unexpected(DomainError::EmptyIdentifier));
}

TEST(NodeId, RejectsNamesThatAreNotDnsLabels) {
    for (const std::string_view name :
         {"-gateway", "gateway-", "-", "Gateway-0", "gateway.ns", "gateway_0", "gateway 0"}) {
        EXPECT_EQ(NodeId::parse(name), std::unexpected(DomainError::MalformedIdentifier)) << name;
    }
}

} // namespace
