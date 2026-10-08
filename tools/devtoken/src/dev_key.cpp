#include "devtoken/dev_key.hpp"

#include "core/util/json.hpp"
#include "core/util/parse.hpp"
#include "core/util/time.hpp"
#include "infra/auth/base64url.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace devtoken {

namespace {

// RFC 8032 section 5.1.5: both halves of an Ed25519 key are 32 bytes.
constexpr std::size_t kKeyBytes = 32;
// RFC 8032 section 5.1.6: a signature is R then S, 32 bytes each.
constexpr std::size_t kEd25519SignatureBytes = 64;
// FIPS 180-4: SHA-256 digests are 256 bits.
constexpr std::size_t kSha256Bytes = 32;

struct MdCtxFree {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};

std::optional<std::string_view> string_member(const core::json::Value& object,
                                              std::string_view name) {
    const core::json::Value* v = object.find(name);
    return v == nullptr ? std::nullopt : v->as_string();
}

std::expected<std::string, DevKeyError> thumbprint(std::string_view x) {
    // RFC 7638 section 3.2: the required members only, in lexical order, without whitespace.
    const std::string canonical =
        R"({"crv":"Ed25519","kty":"OKP","x":")" + std::string(x) + R"("})";
    std::array<unsigned char, kSha256Bytes> digest{};
    std::size_t written = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, canonical.data(), canonical.size(), digest.data(),
                     &written) != 1) {
        ERR_clear_error();
        return std::unexpected(DevKeyError::Crypto);
    }
    return infra::auth::encode_base64url(digest);
}

std::expected<std::string, DevKeyError> public_x(EVP_PKEY* key) {
    std::array<unsigned char, kKeyBytes> x{};
    std::size_t len = x.size();
    if (EVP_PKEY_get_raw_public_key(key, x.data(), &len) != 1 || len != x.size()) {
        ERR_clear_error();
        return std::unexpected(DevKeyError::Crypto);
    }
    return infra::auth::encode_base64url(x);
}

} // namespace

struct DevKey::Pkey {
    explicit Pkey(EVP_PKEY* k) noexcept : key(k) {}
    Pkey(const Pkey&) = delete;
    Pkey& operator=(const Pkey&) = delete;
    Pkey(Pkey&&) = delete;
    Pkey& operator=(Pkey&&) = delete;
    ~Pkey() { EVP_PKEY_free(key); }

    EVP_PKEY* key;
};

std::string_view to_string(DevKeyError e) noexcept {
    switch (e) {
    case DevKeyError::Malformed:
        return "not an Ed25519 private JWK";
    case DevKeyError::Mismatch:
        return "key file x is not the public half of its d";
    case DevKeyError::Crypto:
        return "OpenSSL refused the key";
    }
    return "unknown key error";
}

DevKey::DevKey(std::unique_ptr<Pkey> key, std::string x, std::string kid) noexcept
    : key_(std::move(key)), x_(std::move(x)), kid_(std::move(kid)) {}

DevKey::DevKey(DevKey&&) noexcept = default;
DevKey& DevKey::operator=(DevKey&&) noexcept = default;
DevKey::~DevKey() = default;

std::expected<DevKey, DevKeyError> DevKey::adopt(std::unique_ptr<Pkey> key) {
    if (key->key == nullptr) {
        ERR_clear_error();
        return std::unexpected(DevKeyError::Crypto);
    }
    std::expected<std::string, DevKeyError> x = public_x(key->key);
    if (!x) {
        return std::unexpected(x.error());
    }
    std::expected<std::string, DevKeyError> kid = thumbprint(*x);
    if (!kid) {
        return std::unexpected(kid.error());
    }
    return DevKey{std::move(key), std::move(*x), std::move(*kid)};
}

std::expected<DevKey, DevKeyError> DevKey::generate() {
    return adopt(std::make_unique<Pkey>(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519")));
}

std::expected<DevKey, DevKeyError> DevKey::from_private_jwk(std::string_view json) {
    const auto doc = core::json::parse(json);
    if (!doc || string_member(*doc, "kty") != "OKP" || string_member(*doc, "crv") != "Ed25519") {
        return std::unexpected(DevKeyError::Malformed);
    }
    const std::optional<std::string_view> d_text = string_member(*doc, "d");
    const std::optional<std::string_view> x_text = string_member(*doc, "x");
    const std::optional<std::string> d =
        d_text ? infra::auth::decode_base64url(*d_text) : std::nullopt;
    if (!d || d->size() != kKeyBytes || !x_text) {
        return std::unexpected(DevKeyError::Malformed);
    }
    std::expected<DevKey, DevKeyError> key =
        adopt(std::make_unique<Pkey>(EVP_PKEY_new_raw_private_key_ex(
            nullptr, "ED25519", nullptr, reinterpret_cast<const unsigned char*>(d->data()),
            d->size())));
    if (key && key->x_ != *x_text) {
        return std::unexpected(DevKeyError::Mismatch);
    }
    return key;
}

std::expected<std::string, DevKeyError> DevKey::private_jwk() const {
    std::array<unsigned char, kKeyBytes> d{};
    std::size_t len = d.size();
    if (EVP_PKEY_get_raw_private_key(key_->key, d.data(), &len) != 1 || len != d.size()) {
        ERR_clear_error();
        return std::unexpected(DevKeyError::Crypto);
    }
    return R"({"kty":"OKP","crv":"Ed25519","kid":")" + kid_ + R"(","x":")" + x_ + R"(","d":")" +
           infra::auth::encode_base64url(d) + "\"}\n";
}

std::string DevKey::public_jwks() const {
    return R"({"keys":[{"kty":"OKP","crv":"Ed25519","use":"sig","alg":"EdDSA","kid":")" + kid_ +
           R"(","x":")" + x_ + "\"}]}\n";
}

std::expected<std::string, DevKeyError> DevKey::mint(const MintRequest& request,
                                                     core::WallTime now) const {
    const std::int64_t iat =
        std::chrono::duration_cast<core::Seconds>(now.time_since_epoch()).count();
    std::string header = R"({"alg":"EdDSA","typ":"JWT","kid":)";
    core::json::append_string(header, kid_);
    header += '}';

    std::string payload = R"({"iss":)";
    core::json::append_string(payload, request.issuer);
    payload += R"(,"aud":)";
    core::json::append_string(payload, request.audience);
    payload += R"(,"sub":)";
    core::json::append_string(payload, request.subject);
    if (!request.email.empty()) {
        payload += R"(,"email":)";
        core::json::append_string(payload, request.email);
    }
    if (!request.scope.empty()) {
        payload += R"(,"scope":)";
        core::json::append_string(payload, request.scope);
    }
    if (!request.client.empty()) {
        payload += R"(,"azp":)";
        core::json::append_string(payload, request.client);
    }
    payload += R"(,"iat":)";
    payload += std::to_string(iat);
    payload += R"(,"exp":)";
    payload += std::to_string(iat + request.ttl.count());
    payload += '}';

    const std::string input =
        infra::auth::encode_base64url(header) + '.' + infra::auth::encode_base64url(payload);
    const std::unique_ptr<EVP_MD_CTX, MdCtxFree> ctx{EVP_MD_CTX_new()};
    std::array<unsigned char, kEd25519SignatureBytes> signature{};
    std::size_t len = signature.size();
    const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
    if (!ctx ||
        EVP_DigestSignInit_ex(ctx.get(), nullptr, nullptr, nullptr, nullptr, key_->key, nullptr) !=
            1 ||
        EVP_DigestSign(ctx.get(), signature.data(), &len, bytes, input.size()) != 1 ||
        len != signature.size()) {
        ERR_clear_error();
        return std::unexpected(DevKeyError::Crypto);
    }
    return input + '.' + infra::auth::encode_base64url(signature);
}

std::optional<core::Seconds> parse_ttl(std::string_view text) noexcept {
    const std::optional<std::int64_t> value = core::parse_integer<std::int64_t>(text);
    if (!value || *value < 1 || *value > kMaxTtl.count()) {
        return std::nullopt;
    }
    return core::Seconds{*value};
}

} // namespace devtoken
