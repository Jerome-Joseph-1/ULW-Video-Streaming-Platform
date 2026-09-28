#include "signature.hpp"

#include "core/ports/auth.hpp"

#include "jwk.hpp"
#include "jws.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <memory>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <span>
#include <string_view>

namespace infra::auth::detail {

using core::ports::AuthError;

namespace {

struct MdCtxFree {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};

// RFC 7518 section 3.4: r and s, 32 bytes each for P-256.
constexpr std::size_t kP256ScalarBytes = 32;
// RFC 8032 section 5.1.6.
constexpr std::size_t kEd25519SignatureBytes = 64;

// A SEQUENCE of two INTEGERs of up to 33 bytes each (a leading 0x00 keeps a value with its
// top bit set positive), every element behind a 2-byte tag and length.
using EcdsaDer = std::array<unsigned char, 2 + (2 * (2 + kP256ScalarBytes + 1))>;

std::span<const unsigned char> bytes_of(std::string_view s) noexcept {
    // char and unsigned char may alias each other.
    return {reinterpret_cast<const unsigned char*>(s.data()), s.size()};
}

// JOSE sends r || s; OpenSSL verifies the DER form from X9.62. No length exceeds 127, so
// every length octet is the short form.
std::span<const unsigned char> to_der(std::span<const unsigned char> raw, EcdsaDer& der) noexcept {
    std::size_t pos = 2;
    for (std::span<const unsigned char> half :
         {raw.first(kP256ScalarBytes), raw.last(kP256ScalarBytes)}) {
        while (half.size() > 1 && half.front() == 0) {
            half = half.subspan(1);
        }
        const bool pad = (half.front() & 0x80U) != 0;
        der[pos++] = 0x02;
        der[pos++] = static_cast<unsigned char>(half.size() + (pad ? 1 : 0));
        if (pad) {
            der[pos++] = 0x00;
        }
        std::ranges::copy(half, std::span(der).subspan(pos).begin());
        pos += half.size();
    }
    der[0] = 0x30;
    der[1] = static_cast<unsigned char>(pos - 2);
    return std::span(der).first(pos);
}

} // namespace

std::expected<void, AuthError> check_signature(const CompactJws& jws, const PublicKey& key) {
    if (!key_allows(key, jws.alg)) {
        return std::unexpected(AuthError::UnsupportedAlgorithm);
    }
    std::span<const unsigned char> signature = bytes_of(jws.signature);
    const char* digest = "SHA256";
    EcdsaDer der{};
    switch (jws.alg) {
    case Algorithm::RS256:
    case Algorithm::PS256:
        break;
    case Algorithm::ES256:
        if (signature.size() != 2 * kP256ScalarBytes) {
            return std::unexpected(AuthError::BadSignature);
        }
        signature = to_der(signature, der);
        break;
    case Algorithm::EdDSA:
        if (signature.size() != kEd25519SignatureBytes) {
            return std::unexpected(AuthError::BadSignature);
        }
        // Ed25519 hashes internally, and OpenSSL refuses a digest name for it.
        digest = nullptr;
        break;
    }

    const std::unique_ptr<EVP_MD_CTX, MdCtxFree> ctx{EVP_MD_CTX_new()};
    // Owned by ctx.
    EVP_PKEY_CTX* pctx = nullptr;
    bool ok = ctx != nullptr && EVP_DigestVerifyInit_ex(ctx.get(), &pctx, digest, nullptr, nullptr,
                                                        key.pkey.get(), nullptr) == 1;
    if (ok && jws.alg == Algorithm::PS256) {
        // RFC 7518 section 3.5: MGF1 with SHA-256 and a salt as long as the hash. Verifying at
        // exactly that length refuses a signature made with any other.
        ok = EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
             EVP_PKEY_CTX_set_rsa_mgf1_md_name(pctx, "SHA256", nullptr) == 1 &&
             EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) == 1;
    }
    const std::span<const unsigned char> input = bytes_of(jws.signing_input);
    ok = ok && EVP_DigestVerify(ctx.get(), signature.data(), signature.size(), input.data(),
                                input.size()) == 1;
    if (!ok) {
        // A failed verify queues its reasons on this thread, where the next TLS call to read
        // the queue would take them for its own.
        ERR_clear_error();
        return std::unexpected(AuthError::BadSignature);
    }
    return {};
}

} // namespace infra::auth::detail
