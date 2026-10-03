#include "infra/webpush/vapid.hpp"

#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include "p256.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <iterator>
#include <new>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <span>

namespace infra::webpush {

namespace {

struct MdCtxFree {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};
struct SigFree {
    void operator()(ECDSA_SIG* sig) const noexcept { ECDSA_SIG_free(sig); }
};

// RFC 7515 appendix A.3 form of the header, which never changes.
constexpr std::string_view kHeader = R"({"typ":"JWT","alg":"ES256"})";

std::span<const unsigned char> bytes_of(std::string_view text) noexcept {
    // char and unsigned char may alias each other.
    return {reinterpret_cast<const unsigned char*>(text.data()), text.size()};
}

} // namespace

struct VapidKey::Key {
    detail::Pkey pkey;
};

bool is_vapid_subject(std::string_view subject) noexcept {
    const bool scheme = (subject.starts_with("mailto:") && subject.size() > 7) ||
                        (subject.starts_with("https://") && subject.size() > 8);
    return scheme && subject.size() <= kMaxVapidSubject &&
           std::ranges::all_of(subject, [](char c) { return c > ' ' && c < '\x7f'; });
}

VapidKey::VapidKey(std::unique_ptr<Key> key, std::string public_text) noexcept
    : key_(std::move(key)), public_text_(std::move(public_text)) {}
VapidKey::VapidKey(VapidKey&&) noexcept = default;
VapidKey& VapidKey::operator=(VapidKey&&) noexcept = default;
VapidKey::~VapidKey() = default;

std::expected<VapidKey, VapidError> VapidKey::from_base64url(std::string_view text) {
    auto decoded = auth::decode_base64url(text);
    if (!decoded || decoded->size() != kPrivateKeyBytes) {
        if (decoded) {
            OPENSSL_cleanse(decoded->data(), decoded->size());
        }
        return std::unexpected(VapidError::BadKey);
    }
    PrivateKey scalar{};
    std::ranges::copy(bytes_of(*decoded), scalar.begin());
    OPENSSL_cleanse(decoded->data(), decoded->size());
    auto key = from_private(scalar);
    OPENSSL_cleanse(scalar.data(), scalar.size());
    return key;
}

std::expected<VapidKey, VapidError> VapidKey::from_private(const PrivateKey& key) {
    detail::Pkey pkey = detail::import_private(key);
    const auto point = detail::public_point(pkey.get());
    if (!pkey || !point) {
        return std::unexpected(VapidError::BadKey);
    }
    return VapidKey(std::make_unique<Key>(Key{.pkey = std::move(pkey)}),
                    auth::encode_base64url(std::span<const unsigned char>(*point)));
}

std::expected<std::string, VapidError>
VapidKey::token(std::string_view audience, std::string_view subject, core::WallTime expires) const {
    std::string claims = R"({"aud":)";
    core::json::append_string(claims, audience);
    std::format_to(std::back_inserter(claims), R"(,"exp":{},"sub":)",
                   std::chrono::duration_cast<core::Seconds>(expires.time_since_epoch()).count());
    core::json::append_string(claims, subject);
    claims += '}';

    std::string out = auth::encode_base64url(kHeader);
    out += '.';
    auth::append_base64url(out, bytes_of(claims));
    const std::unique_ptr<EVP_MD_CTX, MdCtxFree> ctx{EVP_MD_CTX_new()};
    // An ECDSA P-256 signature in DER is at most 72 bytes.
    std::array<unsigned char, 80> der{};
    std::size_t der_length = der.size();
    if (!ctx ||
        EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key_->pkey.get()) != 1 ||
        EVP_DigestSign(ctx.get(), der.data(), &der_length, bytes_of(out).data(), out.size()) != 1) {
        return std::unexpected(VapidError::Crypto);
    }
    // JWS (RFC 7518 section 3.4) wants r || s, each 32 bytes; OpenSSL writes X9.62's DER.
    const unsigned char* cursor = der.data();
    const std::unique_ptr<ECDSA_SIG, SigFree> sig{
        d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der_length))};
    std::array<unsigned char, 2 * detail::kScalarBytes> raw{};
    if (!sig ||
        BN_bn2binpad(ECDSA_SIG_get0_r(sig.get()), raw.data(), detail::kScalarBytes) !=
            static_cast<int>(detail::kScalarBytes) ||
        BN_bn2binpad(ECDSA_SIG_get0_s(sig.get()), raw.data() + detail::kScalarBytes,
                     detail::kScalarBytes) != static_cast<int>(detail::kScalarBytes)) {
        return std::unexpected(VapidError::Crypto);
    }
    out += '.';
    auth::append_base64url(out, raw);
    return out;
}

std::expected<std::string, VapidError> VapidKey::authorization(std::string_view audience,
                                                               std::string_view subject,
                                                               core::WallTime expires) const {
    auto jwt = token(audience, subject, expires);
    if (!jwt) {
        return std::unexpected(jwt.error());
    }
    return std::format("vapid t={}, k={}", *jwt, public_text_);
}

} // namespace infra::webpush
