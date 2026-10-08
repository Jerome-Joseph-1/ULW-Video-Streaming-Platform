#include "core/util/json.hpp"
#include "infra/auth/claim_rules.hpp"
#include "infra/auth/service_claim.hpp"

#include "claims.hpp"
#include "support/fake_clock.hpp"
#include "test_claims.hpp"

#include <gtest/gtest.h>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace {

using infra::auth::claim_holds;
using infra::auth::ClaimRules;
using infra::auth::read_service_claim;
using infra::auth::detail::check_claims;
using ulw::test::test_payload;

bool holds(std::string_view json, std::string_view value) {
    const auto doc = core::json::parse(json);
    EXPECT_TRUE(doc) << json;
    return doc && claim_holds(*doc, value);
}

TEST(ServiceClaim, UnsetIsOffWithScopeAsTheClaimItWouldRead) {
    const auto off = read_service_claim(std::nullopt, std::nullopt);
    ASSERT_TRUE(off);
    EXPECT_EQ(off->claim, "scope");
    EXPECT_TRUE(off->value.empty());
}

TEST(ServiceClaim, TheScopeAloneUsesTheDefaultClaim) {
    const auto on = read_service_claim(std::nullopt, "ulw:admin");
    ASSERT_TRUE(on);
    EXPECT_EQ(on->claim, "scope");
    EXPECT_EQ(on->value, "ulw:admin");
    const auto roles = read_service_claim("roles", "ulw-service");
    ASSERT_TRUE(roles);
    EXPECT_EQ(roles->claim, "roles");
    EXPECT_EQ(roles->value, "ulw-service");
}

TEST(ServiceClaim, AClaimWithoutAScopeIsRefused) {
    const auto r = read_service_claim("roles", std::nullopt);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().variable, "ULW_SERVICE_CLAIM");
}

// What every shipped config.env carries until the operator sets a scope: the service is off,
// and the process starts.
TEST(ServiceClaim, TheDefaultClaimWithoutAScopeIsOff) {
    const auto off = read_service_claim("scope", std::nullopt);
    ASSERT_TRUE(off);
    EXPECT_EQ(off->claim, "scope");
    EXPECT_TRUE(off->value.empty());
}

TEST(ServiceClaim, AClientIdIsReadAndCheckedInAzpOrClientId) {
    const auto on = read_service_claim(std::nullopt, "ulw:admin", "ulw-backend");
    ASSERT_TRUE(on);
    EXPECT_EQ(on->client_id, "ulw-backend");
    const auto bad = read_service_claim(std::nullopt, "ulw:admin", "has space");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().variable, "ULW_SERVICE_CLIENT_ID");
    const auto names = [](std::string_view json, std::string_view client) {
        const auto doc = core::json::parse(json);
        return doc && infra::auth::names_client(*doc, client);
    };
    EXPECT_TRUE(names(R"({"azp":"ulw-backend"})", "ulw-backend"));
    EXPECT_TRUE(names(R"({"client_id":"ulw-backend"})", "ulw-backend"));
    EXPECT_TRUE(names(R"({})", ""));
    EXPECT_FALSE(names(R"({"azp":"web-app"})", "ulw-backend"));
    EXPECT_FALSE(names(R"({"azp":["ulw-backend"]})", "ulw-backend"));
    EXPECT_FALSE(names(R"({})", "ulw-backend"));

    ClaimRules rules = ulw::test::kTestRules;
    rules.service_claim = "scope";
    rules.service_value = "ulw:admin";
    rules.service_client_id = "ulw-backend";
    const ulw::test::FakeClock clock;
    const auto check = [&](std::initializer_list<ulw::test::ClaimChange> changes) {
        const auto claims = check_claims(test_payload(changes), rules, clock.wall_now());
        EXPECT_TRUE(claims.has_value());
        return claims && claims->is_service;
    };
    EXPECT_TRUE(check({{"scope", R"("ulw:admin")"}, {"azp", R"("ulw-backend")"}}));
    EXPECT_FALSE(check({{"scope", R"("ulw:admin")"}, {"azp", R"("web-app")"}}));
    EXPECT_FALSE(check({{"scope", R"("ulw:admin")"}}));
}

TEST(ServiceClaim, RefusesClaimNamesNoProviderUsesForScopes) {
    using namespace std::string_view_literals;
    for (const std::string_view bad : {"iss"sv, "aud"sv, "exp"sv, "nbf"sv, "iat"sv, "jti"sv,
                                       "has space"sv, R"(quote")"sv, "x\0y"sv}) {
        const auto r = read_service_claim(bad, "ulw:admin");
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().variable, "ULW_SERVICE_CLAIM");
    }
    EXPECT_FALSE(read_service_claim(std::string(65, 'a'), "ulw:admin"));
    EXPECT_TRUE(read_service_claim(std::string(64, 'a'), "ulw:admin"));
    // The subject is allowed: a backend's client id may be what marks it.
    EXPECT_TRUE(read_service_claim("sub", "backend-client"));
    EXPECT_TRUE(read_service_claim("https://example.com/roles", "ulw:admin"));
}

TEST(ServiceClaim, RefusesValuesASpaceSeparatedScopeCouldNotHold) {
    using namespace std::string_view_literals;
    for (const std::string_view bad :
         {"ulw admin"sv, "tab\there"sv, "new\nline"sv, "del\x7f"sv, "caf\xc3\xa9"sv}) {
        const auto r = read_service_claim(std::nullopt, bad);
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().variable, "ULW_SERVICE_SCOPE");
    }
    EXPECT_FALSE(read_service_claim(std::nullopt, std::string(129, 'a')));
    EXPECT_TRUE(read_service_claim(std::nullopt, std::string(128, 'a')));
}

TEST(ClaimHolds, AStringEqualToTheValueOrListingIt) {
    EXPECT_TRUE(holds(R"("ulw:admin")", "ulw:admin"));
    EXPECT_TRUE(holds(R"("openid ulw:admin profile")", "ulw:admin"));
    EXPECT_TRUE(holds(R"("ulw:admin profile")", "ulw:admin"));
    EXPECT_TRUE(holds(R"("openid ulw:admin")", "ulw:admin"));
    EXPECT_FALSE(holds(R"("ulw:admin2")", "ulw:admin"));
    EXPECT_FALSE(holds(R"("ulw:adminprofile")", "ulw:admin"));
    EXPECT_FALSE(holds(R"("xulw:admin")", "ulw:admin"));
    EXPECT_FALSE(holds(R"("")", "ulw:admin"));
    EXPECT_FALSE(holds(R"("  ")", "ulw:admin"));
}

TEST(ClaimHolds, AnArrayWithAnElementThatHoldsIt) {
    EXPECT_TRUE(holds(R"(["viewer","ulw:admin"])", "ulw:admin"));
    EXPECT_TRUE(holds(R"(["openid ulw:admin"])", "ulw:admin"));
    EXPECT_FALSE(holds(R"(["viewer",1,null])", "ulw:admin"));
    EXPECT_FALSE(holds(R"([])", "ulw:admin"));
}

TEST(ClaimHolds, TrueOnlyForTheValueTrue) {
    EXPECT_TRUE(holds("true", "true"));
    EXPECT_FALSE(holds("false", "true"));
    EXPECT_FALSE(holds("true", "ulw:admin"));
}

TEST(ClaimHolds, NoOtherShapeAndNoEmptyValueHoldsAnything) {
    EXPECT_FALSE(holds("1", "1"));
    EXPECT_FALSE(holds("null", "null"));
    EXPECT_FALSE(holds(R"({"ulw:admin":true})", "ulw:admin"));
    EXPECT_FALSE(holds(R"("")", ""));
    EXPECT_FALSE(holds(R"([""])", ""));
}

class ServiceTokens : public ::testing::Test {
protected:
    [[nodiscard]] bool is_service(const ClaimRules& rules,
                                  std::initializer_list<ulw::test::ClaimChange> changes) const {
        const auto claims = check_claims(test_payload(changes), rules, clock_.wall_now());
        EXPECT_TRUE(claims.has_value());
        return claims && claims->is_service;
    }

    ulw::test::FakeClock clock_;
};

TEST_F(ServiceTokens, NoTokenIsTheServiceWhereNoneIsConfigured) {
    const ClaimRules& rules = ulw::test::kTestRules;
    EXPECT_FALSE(is_service(rules, {{"scope", R"("ulw:admin")"}}));
}

TEST_F(ServiceTokens, ATokenWhoseClaimHoldsTheValueIsTheService) {
    ClaimRules rules = ulw::test::kTestRules;
    rules.service_claim = "scope";
    rules.service_value = "ulw:admin";
    EXPECT_TRUE(is_service(rules, {{"scope", R"("ulw:admin")"}}));
    EXPECT_TRUE(is_service(rules, {{"scope", R"("openid ulw:admin")"}}));
    // Anything else is a user's token, which still serves its user.
    EXPECT_FALSE(is_service(rules, {}));
    EXPECT_FALSE(is_service(rules, {{"scope", R"("openid")"}}));
    EXPECT_FALSE(is_service(rules, {{"roles", R"(["ulw:admin"])"}}));
    rules.service_claim = "roles";
    EXPECT_TRUE(is_service(rules, {{"roles", R"(["ulw:admin"])"}}));
    EXPECT_FALSE(is_service(rules, {{"scope", R"("ulw:admin")"}}));
}

// ADR-0100: who may create uploads, read and matched as the service's claim is.
TEST(UploaderClaim, UnsetIsOffWithScopeAsTheClaimItWouldRead) {
    const auto off = infra::auth::read_uploader_claim(std::nullopt, std::nullopt);
    ASSERT_TRUE(off);
    EXPECT_EQ(off->claim, "scope");
    EXPECT_TRUE(off->value.empty());
    const auto named = infra::auth::read_uploader_claim("scope", std::nullopt);
    ASSERT_TRUE(named);
    EXPECT_TRUE(named->value.empty());
    const auto on = infra::auth::read_uploader_claim("roles", "uploader");
    ASSERT_TRUE(on);
    EXPECT_EQ(on->claim, "roles");
    EXPECT_EQ(on->value, "uploader");
}

TEST(UploaderClaim, RefusesWhatTheServiceClaimRefusesUnderItsOwnNames) {
    const auto alone = infra::auth::read_uploader_claim("roles", std::nullopt);
    ASSERT_FALSE(alone);
    EXPECT_EQ(alone.error().variable, "ULW_UPLOADER_CLAIM");
    EXPECT_NE(alone.error().reason.find("ULW_UPLOADER_SCOPE"), std::string::npos);
    for (const std::string_view bad : {"exp", "has space", "a=b"}) {
        const auto r = infra::auth::read_uploader_claim(bad, "uploader");
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().variable, "ULW_UPLOADER_CLAIM");
    }
    const auto value = infra::auth::read_uploader_claim(std::nullopt, "two words");
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error().variable, "ULW_UPLOADER_SCOPE");
    EXPECT_FALSE(infra::auth::read_uploader_claim(std::nullopt, std::string(129, 'a')));
}

class UploaderTokens : public ServiceTokens {
protected:
    [[nodiscard]] bool may_upload(const ClaimRules& rules,
                                  std::initializer_list<ulw::test::ClaimChange> changes) const {
        const auto claims = check_claims(test_payload(changes), rules, clock_.wall_now());
        EXPECT_TRUE(claims.has_value());
        return claims && claims->may_upload;
    }
};

TEST_F(UploaderTokens, EveryTokenMayUploadWhereNothingIsAsked) {
    EXPECT_TRUE(may_upload(ulw::test::kTestRules, {}));
}

TEST_F(UploaderTokens, OnlyATokenWhoseClaimHoldsTheValueMayUpload) {
    ClaimRules rules = ulw::test::kTestRules;
    rules.uploader_claim = "scope";
    rules.uploader_value = "video:upload";
    EXPECT_TRUE(may_upload(rules, {{"scope", R"("openid video:upload")"}}));
    EXPECT_TRUE(may_upload(rules, {{"scope", R"(["video:upload"])"}}));
    // Anything else still verifies, and serves its user everywhere but a new upload.
    EXPECT_FALSE(may_upload(rules, {}));
    EXPECT_FALSE(may_upload(rules, {{"scope", R"("openid")"}}));
    EXPECT_FALSE(may_upload(rules, {{"scope", R"(42)"}}));
    rules.uploader_claim = "roles";
    rules.uploader_value = "true";
    EXPECT_TRUE(may_upload(rules, {{"roles", "true"}}));
    EXPECT_FALSE(may_upload(rules, {{"roles", "false"}}));
}

} // namespace
