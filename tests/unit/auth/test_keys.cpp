#include "test_keys.hpp"

#include "infra/auth/base64url.hpp"

#include <array>
#include <initializer_list>
#include <memory>
#include <openssl/bn.h>
#include <openssl/core_names.h>
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

} // namespace ulw::test
