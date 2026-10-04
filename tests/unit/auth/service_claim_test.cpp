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

} // namespace
