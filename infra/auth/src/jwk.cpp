#include "jwk.hpp"

#include "core/util/json.hpp"
#include "infra/auth/base64url.hpp"

#include "json_member.hpp"
#include "jws.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::auth::detail {

void PkeyFree::operator()(EVP_PKEY* key) const noexcept {
    EVP_PKEY_free(key);
}

namespace {

struct ParamBuildFree {
    void operator()(OSSL_PARAM_BLD* bld) const noexcept { OSSL_PARAM_BLD_free(bld); }
};
struct ParamFree {
    void operator()(OSSL_PARAM* params) const noexcept { OSSL_PARAM_free(params); }
};
struct PkeyCtxFree {
    void operator()(EVP_PKEY_CTX* ctx) const noexcept { EVP_PKEY_CTX_free(ctx); }
};
struct BignumFree {
    void operator()(BIGNUM* bn) const noexcept { BN_free(bn); }
};
using ParamBuild = std::unique_ptr<OSSL_PARAM_BLD, ParamBuildFree>;
using Bignum = std::unique_ptr<BIGNUM, BignumFree>;

// NIST SP 800-131A disallows RSA signatures below 2048 bits (under 112-bit strength) after
// 2013.
constexpr int kMinRsaBits = 2048;
// RFC 7518 sections 6.2.1.2 and 6.2.1.3: P-256 coordinates are exactly 32 octets. RFC 8037
// section 2: an Ed25519 public key is 32 octets.
constexpr std::size_t kCoordinateBytes = 32;

const unsigned char* bytes_of(const std::string& s) noexcept {
    // char and unsigned char may alias each other.
    return reinterpret_cast<const unsigned char*>(s.data());
}

std::optional<std::string> binary_member(const core::json::Value& object, std::string_view name) {
    const std::optional<std::string_view> text = string_member(object, name);
    if (!text) {
        return std::nullopt;
    }
    std::optional<std::string> bytes = decode_base64url(*text);
    if (!bytes || bytes->empty()) {
        return std::nullopt;
    }
    return bytes;
}

// OpenSSL imports RSA integers as they come. The public-key check is what refuses an even
// modulus or a public exponent of 1, under which any padded digest is its own signature.
Pkey import_public(const char* type, OSSL_PARAM_BLD* bld) {
    const std::unique_ptr<OSSL_PARAM, ParamFree> params{OSSL_PARAM_BLD_to_param(bld)};
    const std::unique_ptr<EVP_PKEY_CTX, PkeyCtxFree> ctx{
        EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr)};
    EVP_PKEY* raw = nullptr;
    if (!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1 ||
        EVP_PKEY_fromdata(ctx.get(), &raw, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
        return {};
    }
    Pkey key{raw};
    const std::unique_ptr<EVP_PKEY_CTX, PkeyCtxFree> check{
        EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr)};
    if (!check || EVP_PKEY_public_check(check.get()) != 1) {
        return {};
    }
    return key;
}

Pkey rsa_key(const core::json::Value& jwk) {
    const std::optional<std::string> n = binary_member(jwk, "n");
    const std::optional<std::string> e = binary_member(jwk, "e");
    if (!n || !e) {
        return {};
    }
    const Bignum bn_n{BN_bin2bn(bytes_of(*n), static_cast<int>(n->size()), nullptr)};
    const Bignum bn_e{BN_bin2bn(bytes_of(*e), static_cast<int>(e->size()), nullptr)};
    const ParamBuild bld{OSSL_PARAM_BLD_new()};
    if (!bn_n || !bn_e || !bld ||
        OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_N, bn_n.get()) != 1 ||
        OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_E, bn_e.get()) != 1) {
        return {};
    }
    Pkey key = import_public("RSA", bld.get());
    if (!key || EVP_PKEY_get_bits(key.get()) < kMinRsaBits) {
        return {};
    }
    return key;
}

Pkey p256_key(const core::json::Value& jwk) {
    const std::optional<std::string> x = binary_member(jwk, "x");
    const std::optional<std::string> y = binary_member(jwk, "y");
    if (!x || !y || x->size() != kCoordinateBytes || y->size() != kCoordinateBytes) {
        return {};
    }
    // SEC 1 section 2.3.3 uncompressed form: 0x04, then x, then y.
    const std::string point = '\x04' + *x + *y;
    const ParamBuild bld{OSSL_PARAM_BLD_new()};
    if (!bld ||
        OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, "P-256", 0) != 1 ||
        OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(),
                                         point.size()) != 1) {
        return {};
    }
    return import_public("EC", bld.get());
}

Pkey ed25519_key(const core::json::Value& jwk) {
    const std::optional<std::string> x = binary_member(jwk, "x");
    const ParamBuild bld{OSSL_PARAM_BLD_new()};
    if (!x || x->size() != kCoordinateBytes || !bld ||
        OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, x->data(),
                                         x->size()) != 1) {
        return {};
    }
    return import_public("ED25519", bld.get());
}

std::optional<KeyType> key_type(const core::json::Value& jwk) noexcept {
    const std::optional<std::string_view> kty = string_member(jwk, "kty");
    const std::optional<std::string_view> crv = string_member(jwk, "crv");
    if (kty == "RSA") {
        return KeyType::Rsa;
    }
    if (kty == "EC" && crv == "P-256") {
        return KeyType::P256;
    }
    if (kty == "OKP" && crv == "Ed25519") {
        return KeyType::Ed25519;
    }
    return std::nullopt;
}

// RFC 7517 sections 4.2 and 4.3: a key published for encryption, or for operations that do
// not include verifying, is not a signing key whatever its type.
bool published_for_verification(const core::json::Value& jwk) noexcept {
    if (const core::json::Value* use = jwk.find("use");
        use != nullptr && use->as_string() != "sig") {
        return false;
    }
    const core::json::Value* ops = jwk.find("key_ops");
    if (ops == nullptr) {
        return true;
    }
    const std::vector<core::json::Value>* list = ops->as_array();
    return list != nullptr && std::ranges::any_of(*list, [](const core::json::Value& op) {
               return op.as_string() == "verify";
           });
}

std::optional<PublicKey> parse_key(const core::json::Value& jwk) {
    const std::optional<std::string_view> kid = string_member(jwk, "kid");
    const std::optional<KeyType> type = key_type(jwk);
    if (!kid || kid->empty() || kid->size() > kMaxKidBytes || !type ||
        !published_for_verification(jwk)) {
        return std::nullopt;
    }
    PublicKey key{.kid = std::string(*kid), .type = *type, .alg = std::nullopt, .pkey = {}};
    if (const core::json::Value* alg = jwk.find("alg"); alg != nullptr) {
        const std::optional<std::string_view> name = alg->as_string();
        key.alg = name ? parse_algorithm(*name) : std::nullopt;
        if (!key.alg) {
            return std::nullopt;
        }
    }
    switch (*type) {
    case KeyType::Rsa:
        key.pkey = rsa_key(jwk);
        break;
    case KeyType::P256:
        key.pkey = p256_key(jwk);
        break;
    case KeyType::Ed25519:
        key.pkey = ed25519_key(jwk);
        break;
    }
    // A key OpenSSL refused leaves its reasons queued on this thread, where the next TLS
    // call to look at the queue would take them for its own.
    ERR_clear_error();
    if (!key.pkey || (key.alg && !key_allows(key, *key.alg))) {
        return std::nullopt;
    }
    return key;
}

} // namespace

bool key_allows(const PublicKey& key, Algorithm alg) noexcept {
    if (key.alg && *key.alg != alg) {
        return false;
    }
    switch (key.type) {
    case KeyType::Rsa:
        return alg == Algorithm::RS256 || alg == Algorithm::PS256;
    case KeyType::P256:
        return alg == Algorithm::ES256;
    case KeyType::Ed25519:
        return alg == Algorithm::EdDSA;
    }
    return false;
}

bool same_key(const PublicKey& a, const PublicKey& b) noexcept {
    return a.type == b.type && a.alg == b.alg && EVP_PKEY_eq(a.pkey.get(), b.pkey.get()) == 1;
}

const PublicKey* KeySet::find(std::string_view kid) const noexcept {
    const auto it = std::ranges::find(keys, kid, &PublicKey::kid);
    return it == keys.end() ? nullptr : &*it;
}

std::optional<KeySet> parse_key_set(std::string_view json) {
    const auto doc = core::json::parse(json);
    if (!doc) {
        return std::nullopt;
    }
    const core::json::Value* keys = doc->find("keys");
    const std::vector<core::json::Value>* list = keys == nullptr ? nullptr : keys->as_array();
    if (list == nullptr) {
        return std::nullopt;
    }
    KeySet set;
    for (const core::json::Value& jwk : *list) {
        std::optional<PublicKey> key = jwk.as_object() == nullptr ? std::nullopt : parse_key(jwk);
        // RFC 7517 section 4.5 wants kids distinct within a set. A repeat is a publishing
        // mistake, and the first key under the kid is the one kept.
        if (!key || set.find(key->kid) != nullptr) {
            ++set.skipped;
            continue;
        }
        set.keys.push_back(std::move(*key));
    }
    return set;
}

} // namespace infra::auth::detail
