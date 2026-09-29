#include "test_keys.hpp"

#include "infra/auth/base64url.hpp"

#include <array>
#include <initializer_list>
#include <memory>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/encoder.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ulw::test {

namespace {

struct CtxFree {
    void operator()(EVP_PKEY_CTX* ctx) const noexcept { EVP_PKEY_CTX_free(ctx); }
};
struct BnFree {
    void operator()(BIGNUM* bn) const noexcept { BN_free(bn); }
};
struct MdCtxFree {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};
struct EcdsaSigFree {
    void operator()(ECDSA_SIG* sig) const noexcept { ECDSA_SIG_free(sig); }
};
struct EncoderFree {
    void operator()(OSSL_ENCODER_CTX* ctx) const noexcept { OSSL_ENCODER_CTX_free(ctx); }
};
using Ctx = std::unique_ptr<EVP_PKEY_CTX, CtxFree>;

void require(bool ok, const char* what) {
    if (!ok) {
        throw std::runtime_error(what);
    }
}

Ctx keygen_ctx(const char* type) {
    Ctx ctx{EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr)};
    require(ctx != nullptr && EVP_PKEY_keygen_init(ctx.get()) == 1, "keygen init");
    return ctx;
}

EVP_PKEY* generate(EVP_PKEY_CTX* ctx) {
    EVP_PKEY* key = nullptr;
    require(EVP_PKEY_generate(ctx, &key) == 1, "keygen");
    return key;
}

std::string b64(const std::vector<unsigned char>& bytes) {
    return infra::auth::encode_base64url(bytes);
}

std::vector<unsigned char> bn_param(const EVP_PKEY* key, const char* name, int pad_to = 0) {
    BIGNUM* raw = nullptr;
    require(EVP_PKEY_get_bn_param(key, name, &raw) == 1, "get bn param");
    const std::unique_ptr<BIGNUM, BnFree> bn{raw};
    const int size = pad_to > 0 ? pad_to : BN_num_bytes(bn.get());
    std::vector<unsigned char> out(static_cast<std::size_t>(size));
    require(BN_bn2binpad(bn.get(), out.data(), size) == size, "bn2bin");
    return out;
}

const unsigned char* bytes_of(std::string_view s) {
    return reinterpret_cast<const unsigned char*>(s.data());
}

// OpenSSL signs ECDSA in DER; JOSE wants the two 32-byte scalars back to back.
std::string der_to_jose(const std::string& der) {
    const unsigned char* p = bytes_of(der);
    const std::unique_ptr<ECDSA_SIG, EcdsaSigFree> sig{
        d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()))};
    require(sig != nullptr, "ecdsa der");
    std::string out(64, '\0');
    auto* dst = reinterpret_cast<unsigned char*>(out.data());
    require(BN_bn2binpad(ECDSA_SIG_get0_r(sig.get()), dst, 32) == 32 &&
                BN_bn2binpad(ECDSA_SIG_get0_s(sig.get()), dst + 32, 32) == 32,
            "ecdsa scalars");
    return out;
}

} // namespace

void TestKey::Free::operator()(EVP_PKEY* key) const noexcept {
    EVP_PKEY_free(key);
}

TestKey::TestKey(std::string kid, EVP_PKEY* key) : kid_(std::move(kid)), key_(key) {}

TestKey TestKey::rsa(std::string kid, int bits) {
    const Ctx ctx = keygen_ctx("RSA");
    require(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), bits) == 1, "rsa bits");
    return {std::move(kid), generate(ctx.get())};
}

TestKey TestKey::p256(std::string kid) {
    const Ctx ctx = keygen_ctx("EC");
    require(EVP_PKEY_CTX_set_group_name(ctx.get(), "P-256") == 1, "ec group");
    return {std::move(kid), generate(ctx.get())};
}

TestKey TestKey::ed25519(std::string kid) {
    const Ctx ctx = keygen_ctx("ED25519");
    return {std::move(kid), generate(ctx.get())};
}

std::string TestKey::jwk(std::string_view extra) const {
    std::string out = R"({"kid":")" + kid_ + '"';
    if (EVP_PKEY_is_a(key_.get(), "RSA") == 1) {
        out += R"(,"kty":"RSA","n":")" + b64(bn_param(key_.get(), OSSL_PKEY_PARAM_RSA_N)) +
               R"(","e":")" + b64(bn_param(key_.get(), OSSL_PKEY_PARAM_RSA_E)) + '"';
    } else if (EVP_PKEY_is_a(key_.get(), "EC") == 1) {
        constexpr int kCoordinate = 32;
        out += R"(,"kty":"EC","crv":"P-256","x":")" +
               b64(bn_param(key_.get(), OSSL_PKEY_PARAM_EC_PUB_X, kCoordinate)) + R"(","y":")" +
               b64(bn_param(key_.get(), OSSL_PKEY_PARAM_EC_PUB_Y, kCoordinate)) + '"';
    } else {
        std::array<unsigned char, 32> x{};
        std::size_t len = x.size();
        require(EVP_PKEY_get_raw_public_key(key_.get(), x.data(), &len) == 1, "raw public key");
        out += R"(,"kty":"OKP","crv":"Ed25519","x":")" + infra::auth::encode_base64url(x) + '"';
    }
    out += extra;
    out += '}';
    return out;
}

std::string TestKey::sign(std::string_view alg, std::string_view input, int pss_salt) const {
    const std::unique_ptr<EVP_MD_CTX, MdCtxFree> ctx{EVP_MD_CTX_new()};
    EVP_PKEY_CTX* pctx = nullptr;
    const char* digest = alg == "EdDSA" ? nullptr : "SHA256";
    require(ctx != nullptr && EVP_DigestSignInit_ex(ctx.get(), &pctx, digest, nullptr, nullptr,
                                                    key_.get(), nullptr) == 1,
            "sign init");
    if (alg == "PS256") {
        require(EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
                    EVP_PKEY_CTX_set_rsa_mgf1_md_name(pctx, "SHA256", nullptr) == 1 &&
                    EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, pss_salt) == 1,
                "pss params");
    }
    std::size_t len = 0;
    require(EVP_DigestSign(ctx.get(), nullptr, &len, bytes_of(input), input.size()) == 1,
            "sign size");
    std::string sig(len, '\0');
    require(EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(sig.data()), &len,
                           bytes_of(input), input.size()) == 1,
            "sign");
    sig.resize(len);
    return alg == "ES256" ? der_to_jose(sig) : sig;
}

std::string TestKey::public_pem() const {
    const std::unique_ptr<OSSL_ENCODER_CTX, EncoderFree> ctx{OSSL_ENCODER_CTX_new_for_pkey(
        key_.get(), EVP_PKEY_PUBLIC_KEY, "PEM", "SubjectPublicKeyInfo", nullptr)};
    unsigned char* data = nullptr;
    std::size_t len = 0;
    require(ctx != nullptr && OSSL_ENCODER_to_data(ctx.get(), &data, &len) == 1, "pem");
    std::string pem(reinterpret_cast<const char*>(data), len);
    OPENSSL_free(data);
    return pem;
}

std::string key_set(std::initializer_list<std::string_view> jwks) {
    std::string out = R"({"keys":[)";
    bool first = true;
    for (const std::string_view jwk : jwks) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += jwk;
    }
    out += "]}";
    return out;
}

std::string compact(std::string_view header_json, std::string_view payload_json,
                    std::string_view signature) {
    return infra::auth::encode_base64url(header_json) + '.' +
           infra::auth::encode_base64url(payload_json) + '.' +
           infra::auth::encode_base64url(signature);
}

std::string signed_token(const TestKey& key, std::string_view alg, std::string_view payload_json) {
    const std::string header =
        R"({"alg":")" + std::string(alg) + R"(","kid":")" + key.kid() + R"("})";
    const std::string input =
        infra::auth::encode_base64url(header) + '.' + infra::auth::encode_base64url(payload_json);
    return input + '.' + infra::auth::encode_base64url(key.sign(alg, input));
}

std::string hmac_sha256(std::string_view secret, std::string_view input) {
    std::array<unsigned char, 32> out{};
    std::size_t len = 0;
    require(EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, bytes_of(secret), secret.size(),
                      bytes_of(input), input.size(), out.data(), out.size(), &len) != nullptr,
            "hmac");
    return {reinterpret_cast<const char*>(out.data()), len};
}

} // namespace ulw::test
