#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"
#include "infra/webpush/vapid.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace infra::webpush;

// RFC 8291's sender key, which is as good a P-256 key as any.
constexpr std::string_view kPrivate = "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw";
constexpr std::string_view kPublic =
    "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8";
// 2026-01-01T12:00:00Z
const core::WallTime kExpires{std::chrono::seconds(1767268800)};

std::vector<std::string> split(std::string_view jwt) {
    std::vector<std::string> parts;
    while (true) {
        const std::size_t dot = jwt.find('.');
        parts.emplace_back(jwt.substr(0, dot));
        if (dot == std::string_view::npos) {
            return parts;
        }
        jwt.remove_prefix(dot + 1);
    }
}

// Checks an ES256 JWS signature (r || s) the way a push service does. OpenSSL's C interface
// takes raw bytes.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
bool verifies(std::string_view signing_input, const std::string& raw_signature,
              const std::string& public_point) {
    struct Free {
        void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); }
        void operator()(EVP_PKEY_CTX* c) const { EVP_PKEY_CTX_free(c); }
        void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); }
        void operator()(OSSL_PARAM_BLD* b) const { OSSL_PARAM_BLD_free(b); }
        void operator()(OSSL_PARAM* p) const { OSSL_PARAM_free(p); }
        void operator()(ECDSA_SIG* s) const { ECDSA_SIG_free(s); }
    };
    const std::unique_ptr<OSSL_PARAM_BLD, Free> bld{OSSL_PARAM_BLD_new()};
    OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, "P-256", 0);
    OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, public_point.data(),
                                     public_point.size());
    const std::unique_ptr<OSSL_PARAM, Free> params{OSSL_PARAM_BLD_to_param(bld.get())};
    const std::unique_ptr<EVP_PKEY_CTX, Free> ctx{
        EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)};
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_fromdata_init(ctx.get()) != 1 ||
        EVP_PKEY_fromdata(ctx.get(), &raw, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
        return false;
    }
    const std::unique_ptr<EVP_PKEY, Free> key{raw};
    if (raw_signature.size() != 64) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(raw_signature.data());
    const std::unique_ptr<ECDSA_SIG, Free> sig{ECDSA_SIG_new()};
    ECDSA_SIG_set0(sig.get(), BN_bin2bn(bytes, 32, nullptr), BN_bin2bn(bytes + 32, 32, nullptr));
    unsigned char* der = nullptr;
    const int der_length = i2d_ECDSA_SIG(sig.get(), &der);
    const std::unique_ptr<EVP_MD_CTX, Free> md{EVP_MD_CTX_new()};
    const bool ok =
        EVP_DigestVerifyInit(md.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1 &&
        EVP_DigestVerify(md.get(), der, static_cast<std::size_t>(der_length),
                         reinterpret_cast<const unsigned char*>(signing_input.data()),
                         signing_input.size()) == 1;
    OPENSSL_free(der);
    return ok;
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)

TEST(Vapid, ThePublicKeyIsTheUncompressedPointOfThePrivateOne) {
    const auto key = VapidKey::from_base64url(kPrivate);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->public_key(), kPublic);
}

TEST(Vapid, TheTokenIsAnEs256JwtForTheAudienceThePushServiceChecks) {
    const auto key = VapidKey::from_base64url(kPrivate);
    ASSERT_TRUE(key.has_value());
    const auto jwt = key->token("https://fcm.googleapis.com", "mailto:ops@example.com", kExpires);
    ASSERT_TRUE(jwt.has_value());
    const auto parts = split(*jwt);
    ASSERT_EQ(parts.size(), 3U);
    const auto header = core::json::parse(infra::auth::decode_base64url(parts[0]).value_or(""));
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->find("alg")->as_string(), "ES256");
    EXPECT_EQ(header->find("typ")->as_string(), "JWT");
    const auto claims = core::json::parse(infra::auth::decode_base64url(parts[1]).value_or(""));
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->find("aud")->as_string(), "https://fcm.googleapis.com");
    EXPECT_EQ(claims->find("sub")->as_string(), "mailto:ops@example.com");
    EXPECT_EQ(claims->find("exp")->as_u64(), 1767268800U);
    EXPECT_EQ(claims->as_object()->size(), 3U);
    const auto signature = infra::auth::decode_base64url(parts[2]);
    ASSERT_TRUE(signature.has_value());
    const std::string signing_input = parts[0] + "." + parts[1];
    const std::string point = infra::auth::decode_base64url(kPublic).value_or("");
    EXPECT_TRUE(verifies(signing_input, *signature, point));
    // And not for anything else.
    EXPECT_FALSE(verifies(signing_input + "x", *signature, point));
}

TEST(Vapid, TheAuthorizationHeaderCarriesTheTokenAndTheKey) {
    const auto key = VapidKey::from_base64url(kPrivate);
    ASSERT_TRUE(key.has_value());
    const auto value = key->authorization("https://updates.push.services.mozilla.com",
                                          "https://example.com", kExpires);
    ASSERT_TRUE(value.has_value());
    ASSERT_TRUE(value->starts_with("vapid t=ey"));
    EXPECT_TRUE(value->ends_with(", k=" + std::string(kPublic)));
    const std::string token = value->substr(8, value->find(',') - 8);
    EXPECT_EQ(split(token).size(), 3U);
}

TEST(Vapid, ASubjectWithAQuoteIsEscapedNotInjected) {
    const auto key = VapidKey::from_base64url(kPrivate);
    ASSERT_TRUE(key.has_value());
    const auto jwt = key->token("https://a.example", R"(mailto:a"b@example.com)", kExpires);
    ASSERT_TRUE(jwt.has_value());
    const auto claims =
        core::json::parse(infra::auth::decode_base64url(split(*jwt)[1]).value_or(""));
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->find("sub")->as_string(), R"(mailto:a"b@example.com)");
}

TEST(Vapid, RefusesWhatIsNotAPrivateKey) {
    EXPECT_EQ(VapidKey::from_base64url("").error(), VapidError::BadKey);
    EXPECT_EQ(VapidKey::from_base64url("not base64url!").error(), VapidError::BadKey);
    // 31 and 33 bytes.
    EXPECT_FALSE(VapidKey::from_base64url(infra::auth::encode_base64url(std::string(31, 'a'))));
    EXPECT_FALSE(VapidKey::from_base64url(infra::auth::encode_base64url(std::string(33, 'a'))));
    // Zero, and the group order (n), are not scalars of the group.
    EXPECT_FALSE(VapidKey::from_private(PrivateKey{}));
    const PrivateKey order{0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
                           0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17,
                           0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51};
    EXPECT_FALSE(VapidKey::from_private(order));
    PrivateKey below = order;
    below[31] = 0x50;
    EXPECT_TRUE(VapidKey::from_private(below));
}

TEST(Vapid, AKeyMovesWithItsPublicHalf) {
    auto key = VapidKey::from_base64url(kPrivate);
    ASSERT_TRUE(key.has_value());
    VapidKey moved = std::move(*key);
    EXPECT_EQ(moved.public_key(), kPublic);
    auto other = VapidKey::from_private(PrivateKey{1});
    ASSERT_TRUE(other.has_value());
    moved = std::move(*other);
    EXPECT_NE(moved.public_key(), kPublic);
    EXPECT_TRUE(moved.token("https://a.example", "mailto:a@b", kExpires).has_value());
}

TEST(Vapid, SubjectsAreMailtoOrHttpsContacts) {
    EXPECT_TRUE(is_vapid_subject("mailto:ops@example.com"));
    EXPECT_TRUE(is_vapid_subject("https://example.com/contact"));
    EXPECT_FALSE(is_vapid_subject(""));
    EXPECT_FALSE(is_vapid_subject("mailto:"));
    EXPECT_FALSE(is_vapid_subject("https://"));
    EXPECT_FALSE(is_vapid_subject("http://example.com"));
    EXPECT_FALSE(is_vapid_subject("ops@example.com"));
    EXPECT_FALSE(is_vapid_subject("mailto:a b@example.com"));
    EXPECT_FALSE(is_vapid_subject("mailto:\x01@example.com"));
    EXPECT_FALSE(is_vapid_subject("mailto:" + std::string(kMaxVapidSubject, 'a')));
}

} // namespace
