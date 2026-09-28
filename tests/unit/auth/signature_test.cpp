#include "core/ports/auth.hpp"
#include "infra/auth/base64url.hpp"

#include "jwk.hpp"
#include "jws.hpp"
#include "signature.hpp"
#include "test_keys.hpp"

#include <cstddef>
#include <expected>
#include <gtest/gtest.h>
#include <openssl/err.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using core::ports::AuthError;
using infra::auth::encode_base64url;
using infra::auth::detail::check_signature;
using infra::auth::detail::parse_compact;
using infra::auth::detail::parse_key_set;
using infra::auth::detail::PublicKey;
using ulw::test::compact;
using ulw::test::key_set;
using ulw::test::signed_token;
using ulw::test::TestKey;

constexpr std::string_view kPayload = R"({"sub":"alice","exp":1767229200})";

// Key generation dominates this suite's run time, so each key is made once.
struct KeyTrio {
    TestKey rsa = TestKey::rsa("k");
    TestKey p256 = TestKey::p256("k");
    TestKey ed = TestKey::ed25519("k");

    [[nodiscard]] const TestKey& for_alg(std::string_view alg) const {
        if (alg == "ES256") {
            return p256;
        }
        if (alg == "EdDSA") {
            return ed;
        }
        return rsa;
    }
};

const TestKey& key_for(std::string_view alg) {
    static const KeyTrio keys;
    return keys.for_alg(alg);
}

// Same types, same kid, different key material.
const TestKey& other_key_for(std::string_view alg) {
    static const KeyTrio keys;
    return keys.for_alg(alg);
}

PublicKey load(const std::string& jwk) {
    std::optional<infra::auth::detail::KeySet> set = parse_key_set(key_set({jwk}));
    if (!set || set->keys.size() != 1) {
        throw std::runtime_error("test key did not load: " + jwk);
    }
    return std::move(set->keys.front());
}

std::expected<void, AuthError> check(std::string_view token, const PublicKey& key) {
    const auto jws = parse_compact(token);
    if (!jws) {
        return std::unexpected(jws.error());
    }
    return check_signature(*jws, key);
}

std::string replace_segment(std::string token, int index, std::string_view encoded) {
    std::size_t begin = 0;
    for (int i = 0; i < index; ++i) {
        begin = token.find('.', begin) + 1;
    }
    const std::size_t end = token.find('.', begin);
    token.replace(begin, (end == std::string::npos ? token.size() : end) - begin, encoded);
    return token;
}

class SignatureTest : public ::testing::TestWithParam<std::string_view> {
protected:
    [[nodiscard]] static std::string_view alg() { return GetParam(); }
    [[nodiscard]] static std::string token() {
        return signed_token(key_for(alg()), alg(), kPayload);
    }

    PublicKey key_ = load(key_for(GetParam()).jwk());
};

TEST_P(SignatureTest, VerifiesASignatureMadeWithTheKey) {
    EXPECT_TRUE(check(token(), key_).has_value());
}

TEST_P(SignatureTest, RejectsASignatureMadeWithAnotherKeyOfTheSameType) {
    const std::string forged = signed_token(other_key_for(alg()), alg(), kPayload);
    EXPECT_EQ(check(forged, key_), std::unexpected(AuthError::BadSignature));
}

TEST_P(SignatureTest, RejectsAPayloadChangedAfterSigning) {
    const std::string tampered = replace_segment(
        token(), 1, encode_base64url(std::string_view{R"({"sub":"mallory","exp":1767229200})"}));
    EXPECT_EQ(check(tampered, key_), std::unexpected(AuthError::BadSignature));
}

TEST_P(SignatureTest, RejectsAHeaderChangedAfterSigning) {
    const std::string header = R"({"alg":")" + std::string(alg()) + R"(","kid":"k","typ":"JWT"})";
    const std::string tampered = replace_segment(token(), 0, encode_base64url(header));
    EXPECT_EQ(check(tampered, key_), std::unexpected(AuthError::BadSignature));
}

TEST_P(SignatureTest, RejectsAnyFlippedSignatureBit) {
    const std::string good = token();
    const auto jws = parse_compact(good);
    ASSERT_TRUE(jws.has_value());
    for (const std::size_t byte :
         {std::size_t{0}, jws->signature.size() / 2, jws->signature.size() - 1}) {
        std::string sig = jws->signature;
        sig[byte] = static_cast<char>(sig[byte] ^ 0x01);
        EXPECT_EQ(check(replace_segment(good, 2, encode_base64url(sig)), key_),
                  std::unexpected(AuthError::BadSignature))
            << "byte " << byte;
    }
}

TEST_P(SignatureTest, RejectsATruncatedSignature) {
    const std::string good = token();
    const auto jws = parse_compact(good);
    ASSERT_TRUE(jws.has_value());
    const std::string shorter = jws->signature.substr(0, jws->signature.size() - 1);
    EXPECT_EQ(check(replace_segment(good, 2, encode_base64url(shorter)), key_),
              std::unexpected(AuthError::BadSignature));
}

TEST_P(SignatureTest, LeavesNoOpenSslErrorQueuedAfterARejection) {
    ERR_clear_error();
    const std::string forged = signed_token(other_key_for(alg()), alg(), kPayload);
    ASSERT_FALSE(check(forged, key_).has_value());
    EXPECT_EQ(ERR_peek_error(), 0U);
}

INSTANTIATE_TEST_SUITE_P(Algorithms, SignatureTest,
                         ::testing::Values("RS256", "PS256", "ES256", "EdDSA"),
                         [](const ::testing::TestParamInfo<std::string_view>& param) {
                             return std::string(param.param);
                         });

TEST(SignatureRulesTest, AnRs256SignatureDoesNotPassAsPs256) {
    const TestKey& rsa = key_for("RS256");
    const std::string header = R"({"alg":"PS256","kid":"k"})";
    const std::string input = encode_base64url(header) + '.' + encode_base64url(kPayload);
    const std::string token = input + '.' + encode_base64url(rsa.sign("RS256", input));
    EXPECT_EQ(check(token, load(rsa.jwk())), std::unexpected(AuthError::BadSignature));
}

// RFC 7518 fixes the PSS salt at the hash length; 20 is the SHA-1 length some libraries keep.
TEST(SignatureRulesTest, APs256SignatureWithAnotherSaltLengthIsRejected) {
    const TestKey& rsa = key_for("PS256");
    const std::string header = R"({"alg":"PS256","kid":"k"})";
    const std::string input = encode_base64url(header) + '.' + encode_base64url(kPayload);
    const std::string token = input + '.' + encode_base64url(rsa.sign("PS256", input, 20));
    EXPECT_EQ(check(token, load(rsa.jwk())), std::unexpected(AuthError::BadSignature));
}

TEST(SignatureRulesTest, Es256TakesOnlyTheFixedLengthJoseForm) {
    const PublicKey key = load(key_for("ES256").jwk());
    const std::string header = R"({"alg":"ES256","kid":"k"})";
    // 70 to 72 bytes is what a DER-encoded P-256 signature takes.
    for (const std::size_t size : {63U, 65U, 70U, 72U}) {
        const std::string token = compact(header, kPayload, std::string(size, '\x42'));
        EXPECT_EQ(check(token, key), std::unexpected(AuthError::BadSignature)) << size;
    }
}

// About half of all scalars have the top bit set and one in 256 starts with a zero byte, the
// two cases the DER conversion must get right; enough signatures hit both.
TEST(SignatureRulesTest, EveryEs256SignatureVerifiesWhateverItsScalarsLookLike) {
    const TestKey& p256 = key_for("ES256");
    const PublicKey key = load(p256.jwk());
    for (int i = 0; i < 512; ++i) {
        const std::string payload = R"({"n":)" + std::to_string(i) + '}';
        ASSERT_TRUE(check(signed_token(p256, "ES256", payload), key).has_value()) << i;
    }
}

TEST(SignatureRulesTest, AKeyThatNamesItsAlgorithmRefusesAnyOther) {
    const TestKey& rsa = key_for("RS256");
    const PublicKey pinned = load(rsa.jwk(R"(,"alg":"PS256")"));
    EXPECT_TRUE(check(signed_token(rsa, "PS256", kPayload), pinned).has_value());
    EXPECT_EQ(check(signed_token(rsa, "RS256", kPayload), pinned),
              std::unexpected(AuthError::UnsupportedAlgorithm));
}

TEST(SignatureRulesTest, TheHeaderCannotPickAnAlgorithmOfAnotherKeyType) {
    const PublicKey rsa = load(key_for("RS256").jwk());
    const PublicKey p256 = load(key_for("ES256").jwk());
    const PublicKey ed = load(key_for("EdDSA").jwk());
    EXPECT_EQ(check(signed_token(key_for("ES256"), "ES256", kPayload), rsa),
              std::unexpected(AuthError::UnsupportedAlgorithm));
    EXPECT_EQ(check(signed_token(key_for("EdDSA"), "EdDSA", kPayload), p256),
              std::unexpected(AuthError::UnsupportedAlgorithm));
    EXPECT_EQ(check(signed_token(key_for("RS256"), "RS256", kPayload), ed),
              std::unexpected(AuthError::UnsupportedAlgorithm));
}

// The classic confusion: HMAC keyed with the bytes of the public key, which a verifier that
// let the header pick the algorithm would accept.
TEST(SignatureRulesTest, AnHs256TokenKeyedWithTheRsaPublicKeyIsRejected) {
    const TestKey& rsa = key_for("RS256");
    const std::string header = R"({"alg":"HS256","kid":"k"})";
    const std::string input = encode_base64url(header) + '.' + encode_base64url(kPayload);
    const std::string token =
        input + '.' + encode_base64url(ulw::test::hmac_sha256(rsa.public_pem(), input));
    EXPECT_EQ(check(token, load(rsa.jwk())), std::unexpected(AuthError::UnsupportedAlgorithm));
}

} // namespace
