#pragma once

#include "infra/auth/base64url.hpp"

#include <array>
#include <climits>
#include <cstdint>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ulw::test {

// Fake, as every credential in the tests: it guards nothing.
inline constexpr std::string_view kWebhookKey = "fake-webhook-key";
inline constexpr std::string_view kWebhookSecret = "fake-webhook-secret-for-tests-only-0123456789";

// What LiveKit's notifier puts in a webhook's Authorization header (webhook/url_notifier.go and
// auth/accesstoken.go, protocol of LiveKit v1.13.7), spelled out here independently of the
// gateway's reader: an HS256 JWT with iss, iat, nbf, exp and the body's SHA-256 in standard
// base64.
// NOLINTBEGIN(readability-redundant-member-init): every member has an initializer, so a
// designated initializer may name only those it sets.
struct WebhookToken {
    std::string key = std::string(kWebhookKey);
    std::string secret = std::string(kWebhookSecret);
    std::string alg = "HS256";
    std::int64_t issued = 0;
    std::optional<std::int64_t> expires = std::nullopt;
    std::optional<std::int64_t> not_before = std::nullopt;
    // The body's hash unless set.
    std::optional<std::string> sha256 = std::nullopt;
    // These claims verbatim in place of the ones above.
    std::optional<std::string> claims = std::nullopt;
};
// NOLINTEND(readability-redundant-member-init)

inline std::string body_sha256(std::string_view body) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): OpenSSL takes uchar.
    SHA256(reinterpret_cast<const unsigned char*>(body.data()), body.size(), digest.data());
    std::array<unsigned char, 64> text{};
    const int n = EVP_EncodeBlock(text.data(), digest.data(), static_cast<int>(digest.size()));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char*>(text.data()), static_cast<std::size_t>(n)};
}

inline std::string sign_webhook(std::string_view body, const WebhookToken& t) {
    std::string claims = R"({"iss":")" + t.key + R"(","iat":)" + std::to_string(t.issued);
    if (t.not_before) {
        claims += R"(,"nbf":)" + std::to_string(*t.not_before);
    }
    if (t.expires) {
        claims += R"(,"exp":)" + std::to_string(*t.expires);
    }
    claims += R"(,"sha256":")" + t.sha256.value_or(body_sha256(body)) + R"("})";
    if (t.claims) {
        claims = *t.claims;
    }
    std::string jwt = infra::auth::encode_base64url(R"({"alg":")" + t.alg + R"(","typ":"JWT"})");
    jwt += '.';
    jwt += infra::auth::encode_base64url(claims);
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int size = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* input = reinterpret_cast<const unsigned char*>(jwt.data());
    HMAC(EVP_sha256(), t.secret.data(), static_cast<int>(t.secret.size()), input, jwt.size(),
         mac.data(), &size);
    jwt += '.';
    jwt += infra::auth::encode_base64url(std::span<const unsigned char>(mac.data(), size));
    return jwt;
}

// A token LiveKit would make at `now` (unix seconds): valid for five minutes.
inline std::string livekit_token(std::string_view body, std::int64_t now) {
    return sign_webhook(body, {.issued = now, .expires = now + 300, .not_before = now});
}

} // namespace ulw::test
