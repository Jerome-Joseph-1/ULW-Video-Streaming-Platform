#include "infra/auth/base64url.hpp"
#include "infra/webpush/ece.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace infra::webpush;

std::vector<std::uint8_t> b64(std::string_view text) {
    const auto decoded = infra::auth::decode_base64url(text);
    EXPECT_TRUE(decoded.has_value()) << text;
    const std::string bytes = decoded.value_or("");
    return {bytes.begin(), bytes.end()};
}

template <std::size_t N> std::array<std::uint8_t, N> b64_array(std::string_view text) {
    const auto bytes = b64(text);
    std::array<std::uint8_t, N> out{};
    EXPECT_EQ(bytes.size(), N) << text;
    std::copy_n(bytes.begin(), std::min(N, bytes.size()), out.begin());
    return out;
}

std::span<const std::uint8_t> text_bytes(std::string_view text) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// RFC 8291, section 5 and appendix A.
constexpr std::string_view kPlaintext = "When I grow up, I want to be a watermelon";
constexpr std::string_view kAsPrivate = "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw";
constexpr std::string_view kAsPublic =
    "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8";
constexpr std::string_view kUaPrivate = "q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94";
constexpr std::string_view kUaPublic =
    "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4";
constexpr std::string_view kSalt = "DGv6ra1nlYgDCS1FRnbzlw";
constexpr std::string_view kAuth = "BTBZMqHH6r4Tts7J_aSIgg";
constexpr std::string_view kMessage =
    "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27ml"
    "mlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPT"
    "pK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN";
// The appendix's header and ciphertext, which the message is.
constexpr std::string_view kHeader =
    "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27ml"
    "mlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8";
constexpr std::string_view kCiphertext =
    "8pfeW0KbunFT06SuDKoJH9Ql87S1QUrdirN6GcG7sFz1y1sqLgVi1VhjVkHsUoEsbI_0LpXMuGvnzQ";

TEST(Ece, ReproducesTheRfc8291ExampleExactly) {
    const auto message =
        encrypt_with(b64_array<32>(kAsPrivate), b64_array<16>(kSalt), b64_array<65>(kUaPublic),
                     b64_array<16>(kAuth), text_bytes(kPlaintext));
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(*message, b64(kMessage));
    // 86 of header, 41 of plaintext and its delimiter, 16 of tag. The section 5 request says
    // Content-Length: 145, one more than its own body decodes to; the body is what is checked.
    EXPECT_EQ(message->size(), kHeaderBytes + kPlaintext.size() + 1 + kTagBytes);
    std::vector<std::uint8_t> appendix = b64(kHeader);
    const auto ciphertext = b64(kCiphertext);
    EXPECT_EQ(appendix.size(), kHeaderBytes);
    appendix.insert(appendix.end(), ciphertext.begin(), ciphertext.end());
    EXPECT_EQ(*message, appendix);
    // The header carries the sender's public key as the appendix gives it.
    const auto as_public = b64(kAsPublic);
    EXPECT_TRUE(std::equal(as_public.begin(), as_public.end(), message->begin() + 21));
}

TEST(Ece, DecryptsTheRfc8291Example) {
    const auto plaintext = decrypt(b64_array<32>(kUaPrivate), b64_array<16>(kAuth), b64(kMessage));
    ASSERT_TRUE(plaintext.has_value());
    EXPECT_EQ(std::string(plaintext->begin(), plaintext->end()), kPlaintext);
}

TEST(Ece, AFreshMessageDecryptsAndNeverRepeats) {
    const auto ua = generate_key_pair();
    ASSERT_TRUE(ua.has_value());
    const AuthSecret auth = b64_array<16>(kAuth);
    const auto first = encrypt(ua->public_key, auth, text_bytes("ring"));
    const auto second = encrypt(ua->public_key, auth, text_bytes("ring"));
    ASSERT_TRUE(first && second);
    // A fresh salt and sender key each time.
    EXPECT_NE(*first, *second);
    EXPECT_FALSE(std::equal(first->begin(), first->begin() + kHeaderBytes, second->begin()));
    const auto plaintext = decrypt(ua->private_key, auth, *first);
    ASSERT_TRUE(plaintext.has_value());
    EXPECT_EQ(std::string(plaintext->begin(), plaintext->end()), "ring");
}

TEST(Ece, TheLargestPlaintextFillsOneMessageAndOneMoreIsRefused) {
    const auto ua = generate_key_pair();
    ASSERT_TRUE(ua.has_value());
    const std::vector<std::uint8_t> largest(kMaxPlaintext, 'x');
    const auto message = encrypt(ua->public_key, AuthSecret{}, largest);
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message->size(), 4096U);
    const std::vector<std::uint8_t> over(kMaxPlaintext + 1, 'x');
    EXPECT_EQ(encrypt(ua->public_key, AuthSecret{}, over).error(), EceError::TooLarge);
}

TEST(Ece, RefusesAKeyOffTheCurve) {
    PublicKey off = b64_array<65>(kUaPublic);
    off[64] ^= 1U;
    EXPECT_FALSE(is_p256_point(off));
    EXPECT_TRUE(is_p256_point(b64_array<65>(kUaPublic)));
    EXPECT_EQ(encrypt(off, AuthSecret{}, text_bytes("x")).error(), EceError::BadKey);
    // A compressed point, or the wrong length, is not what a subscription carries.
    PublicKey compressed = b64_array<65>(kUaPublic);
    compressed[0] = 0x02;
    EXPECT_FALSE(is_p256_point(compressed));
    EXPECT_FALSE(is_p256_point(std::span(compressed).first(33)));
}

TEST(Ece, RefusesAPrivateKeyOutOfRange) {
    EXPECT_EQ(
        encrypt_with(PrivateKey{}, Salt{}, b64_array<65>(kUaPublic), AuthSecret{}, text_bytes("x"))
            .error(),
        EceError::BadKey);
    PrivateKey all_ones{};
    all_ones.fill(0xff);
    EXPECT_EQ(decrypt(all_ones, AuthSecret{}, b64(kMessage)).error(), EceError::BadKey);
}

TEST(Ece, DecryptRefusesWhatWasNotSealedForIt) {
    const auto ua_private = b64_array<32>(kUaPrivate);
    const auto auth = b64_array<16>(kAuth);
    auto message = b64(kMessage);
    // Too short to hold a record.
    EXPECT_EQ(decrypt(ua_private, auth, std::span(message).first(kHeaderBytes + 16)).error(),
              EceError::Malformed);
    // Another auth secret, a flipped ciphertext bit, a key length other than 65.
    EXPECT_EQ(decrypt(ua_private, AuthSecret{}, message).error(), EceError::Malformed);
    auto flipped = message;
    flipped.back() ^= 1U;
    EXPECT_EQ(decrypt(ua_private, auth, flipped).error(), EceError::Malformed);
    auto idlen = message;
    idlen[20] = 64;
    EXPECT_EQ(decrypt(ua_private, auth, idlen).error(), EceError::Malformed);
    // A record size smaller than the record.
    auto small_rs = message;
    small_rs[18] = 0;
    small_rs[19] = 10;
    EXPECT_EQ(decrypt(ua_private, auth, small_rs).error(), EceError::Malformed);
    // A sender key that is not a point.
    auto bad_key = message;
    bad_key[22] ^= 1U;
    EXPECT_EQ(decrypt(ua_private, auth, bad_key).error(), EceError::BadKey);
}

TEST(Ece, AnEmptyPlaintextRoundTrips) {
    const auto ua = generate_key_pair();
    ASSERT_TRUE(ua.has_value());
    const auto message = encrypt(ua->public_key, AuthSecret{}, std::vector<std::uint8_t>{});
    ASSERT_TRUE(message.has_value());
    const auto plaintext = decrypt(ua->private_key, AuthSecret{}, *message);
    ASSERT_TRUE(plaintext.has_value());
    EXPECT_TRUE(plaintext->empty());
}

} // namespace
