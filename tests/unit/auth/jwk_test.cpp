#include "infra/auth/base64url.hpp"

#include "jwk.hpp"
#include "jws.hpp"
#include "test_keys.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using infra::auth::detail::Algorithm;
using infra::auth::detail::key_allows;
using infra::auth::detail::KeySet;
using infra::auth::detail::KeyType;
using infra::auth::detail::parse_key_set;
using ulw::test::key_set;
using ulw::test::TestKey;

// Key generation dominates this suite's run time, so each key is made once.
const TestKey& rsa_key() {
    static const TestKey key = TestKey::rsa("rsa");
    return key;
}

const TestKey& p256_key() {
    static const TestKey key = TestKey::p256("ec");
    return key;
}

const TestKey& ed_key() {
    static const TestKey key = TestKey::ed25519("ed");
    return key;
}

class JwkSetTest : public ::testing::Test {
protected:
    static KeySet parse(std::string_view json) {
        std::optional<KeySet> set = parse_key_set(json);
        EXPECT_TRUE(set.has_value()) << json;
        return set ? std::move(*set) : KeySet{};
    }

    const TestKey* rsa_ = &rsa_key();
    const TestKey* p256_ = &p256_key();
    const TestKey* ed_ = &ed_key();
};

TEST_F(JwkSetTest, LoadsRsaP256AndEd25519Keys) {
    const KeySet set = parse(key_set({rsa_->jwk(), p256_->jwk(), ed_->jwk()}));
    EXPECT_EQ(set.skipped, 0U);
    ASSERT_EQ(set.keys.size(), 3U);
    ASSERT_NE(set.find("rsa"), nullptr);
    EXPECT_EQ(set.find("rsa")->type, KeyType::Rsa);
    ASSERT_NE(set.find("ec"), nullptr);
    EXPECT_EQ(set.find("ec")->type, KeyType::P256);
    ASSERT_NE(set.find("ed"), nullptr);
    EXPECT_EQ(set.find("ed")->type, KeyType::Ed25519);
    EXPECT_EQ(set.find("other"), nullptr);
}

TEST_F(JwkSetTest, TheKeyTypeDecidesWhichAlgorithmsItVerifies) {
    const KeySet set = parse(key_set({rsa_->jwk(), p256_->jwk(), ed_->jwk()}));
    ASSERT_EQ(set.keys.size(), 3U);
    const auto& rsa = *set.find("rsa");
    EXPECT_TRUE(key_allows(rsa, Algorithm::RS256));
    EXPECT_TRUE(key_allows(rsa, Algorithm::PS256));
    EXPECT_FALSE(key_allows(rsa, Algorithm::ES256));
    EXPECT_FALSE(key_allows(rsa, Algorithm::EdDSA));
    const auto& ec = *set.find("ec");
    EXPECT_TRUE(key_allows(ec, Algorithm::ES256));
    EXPECT_FALSE(key_allows(ec, Algorithm::RS256));
    EXPECT_FALSE(key_allows(ec, Algorithm::EdDSA));
    const auto& ed = *set.find("ed");
    EXPECT_TRUE(key_allows(ed, Algorithm::EdDSA));
    EXPECT_FALSE(key_allows(ed, Algorithm::ES256));
    EXPECT_FALSE(key_allows(ed, Algorithm::PS256));
}

TEST_F(JwkSetTest, AnAlgInTheJwkNarrowsTheKeyToThatAlgorithm) {
    const KeySet set = parse(key_set({rsa_->jwk(R"(,"alg":"PS256","use":"sig")")}));
    ASSERT_EQ(set.keys.size(), 1U);
    EXPECT_TRUE(key_allows(set.keys[0], Algorithm::PS256));
    EXPECT_FALSE(key_allows(set.keys[0], Algorithm::RS256));
}

TEST_F(JwkSetTest, SkipsKeysWhoseAlgDoesNotFitTheirType) {
    const KeySet set = parse(key_set({
        rsa_->jwk(R"(,"alg":"ES256")"),
        rsa_->jwk(R"(,"alg":"HS256")"),
        rsa_->jwk(R"(,"alg":"RS512")"),
        p256_->jwk(R"(,"alg":"EdDSA")"),
        ed_->jwk(R"(,"alg":"none")"),
        ed_->jwk(R"(,"alg":7)"),
    }));
    EXPECT_TRUE(set.keys.empty());
    EXPECT_EQ(set.skipped, 6U);
}

TEST_F(JwkSetTest, SkipsRsaModuliUnder2048Bits) {
    const TestKey small = TestKey::rsa("small", 1024);
    const KeySet set = parse(key_set({small.jwk(), rsa_->jwk()}));
    EXPECT_EQ(set.find("small"), nullptr);
    EXPECT_NE(set.find("rsa"), nullptr);
    EXPECT_EQ(set.skipped, 1U);
}

TEST_F(JwkSetTest, SkipsUnknownKeyTypesAndCurves) {
    const std::string x = infra::auth::encode_base64url(std::string(32, 'x'));
    const KeySet set = parse(key_set({
        R"({"kid":"hmac","kty":"oct","k":"c2VjcmV0"})",
        R"({"kid":"p384","kty":"EC","crv":"P-384","x":")" + x + R"(","y":")" + x + R"("})",
        R"({"kid":"x25519","kty":"OKP","crv":"X25519","x":")" + x + R"("})",
        R"({"kid":"nocrv","kty":"EC","x":")" + x + R"(","y":")" + x + R"("})",
        R"({"kid":"nokty","n":"AQAB","e":"AQAB"})",
    }));
    EXPECT_TRUE(set.keys.empty());
    EXPECT_EQ(set.skipped, 5U);
}

TEST_F(JwkSetTest, SkipsKeysNotPublishedForVerification) {
    const KeySet set = parse(key_set({
        rsa_->jwk(R"(,"use":"enc")"),
        p256_->jwk(R"(,"key_ops":["encrypt","wrapKey"])"),
        ed_->jwk(R"(,"key_ops":["sign","verify"])"),
    }));
    ASSERT_EQ(set.keys.size(), 1U);
    EXPECT_EQ(set.keys[0].kid, "ed");
    EXPECT_EQ(set.skipped, 2U);
}

TEST_F(JwkSetTest, SkipsKeysWithoutAUsableKid) {
    std::string no_kid = ed_->jwk();
    const std::string member = R"("kid":")" + ed_->kid() + R"(",)";
    no_kid.erase(no_kid.find(member), member.size());
    ASSERT_EQ(no_kid.find("kid"), std::string::npos) << no_kid;
    const KeySet set = parse(key_set({no_kid}));
    EXPECT_TRUE(set.keys.empty());
    EXPECT_EQ(set.skipped, 1U);
}

TEST_F(JwkSetTest, KeepsTheFirstKeyUnderARepeatedKid) {
    const TestKey twin = TestKey::ed25519("ed");
    const KeySet set = parse(key_set({ed_->jwk(), twin.jwk()}));
    ASSERT_EQ(set.keys.size(), 1U);
    EXPECT_EQ(set.skipped, 1U);
    const KeySet only_first = parse(key_set({ed_->jwk()}));
    EXPECT_EQ(EVP_PKEY_eq(set.keys[0].pkey.get(), only_first.keys[0].pkey.get()), 1);
}

TEST_F(JwkSetTest, SkipsPointsOffTheCurveAndCoordinatesOfTheWrongLength) {
    const std::string good = p256_->jwk();
    // Flipping one character of y moves the point off the curve.
    std::string off_curve = good;
    const std::size_t y = off_curve.find(R"("y":")") + 5;
    off_curve[y] = off_curve[y] == 'A' ? 'B' : 'A';
    const std::string short_x = infra::auth::encode_base64url(std::string(31, 'x'));
    const KeySet set = parse(key_set({
        off_curve,
        R"({"kid":"short","kty":"EC","crv":"P-256","x":")" + short_x + R"(","y":")" + short_x +
            R"("})",
        R"({"kid":"short-ed","kty":"OKP","crv":"Ed25519","x":")" + short_x + R"("})",
    }));
    EXPECT_TRUE(set.keys.empty());
    EXPECT_EQ(set.skipped, 3U);
}

TEST_F(JwkSetTest, SkipsRsaKeysWithADegeneratePublicExponent) {
    std::string e_one = rsa_->jwk();
    const std::size_t e = e_one.find(R"("e":")") + 5;
    e_one.replace(e, e_one.find('"', e) - e, "AQ");
    const KeySet set = parse(key_set({e_one}));
    EXPECT_TRUE(set.keys.empty());
    EXPECT_EQ(set.skipped, 1U);
}

TEST_F(JwkSetTest, SkipsMembersThatAreNotObjects) {
    const KeySet set = parse(R"({"keys":[1,"key",null,[]]})");
    EXPECT_TRUE(set.keys.empty());
    EXPECT_EQ(set.skipped, 4U);
}

TEST_F(JwkSetTest, RefusesDocumentsThatAreNotKeySets) {
    EXPECT_FALSE(parse_key_set("not json").has_value());
    EXPECT_FALSE(parse_key_set("[]").has_value());
    EXPECT_FALSE(parse_key_set("{}").has_value());
    EXPECT_FALSE(parse_key_set(R"({"keys":{}})").has_value());
    EXPECT_FALSE(parse_key_set(R"({"keys":[],"keys":[]})").has_value());
}

} // namespace
