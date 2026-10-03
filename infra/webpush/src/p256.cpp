#include "p256.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>

namespace infra::webpush::detail {

namespace {

struct BnFree {
    void operator()(BIGNUM* bn) const noexcept { BN_clear_free(bn); }
};
using Bignum = std::unique_ptr<BIGNUM, BnFree>;
struct GroupFree {
    void operator()(EC_GROUP* group) const noexcept { EC_GROUP_free(group); }
};
struct PointFree {
    void operator()(EC_POINT* point) const noexcept { EC_POINT_free(point); }
};
struct ParamBuildFree {
    void operator()(OSSL_PARAM_BLD* bld) const noexcept { OSSL_PARAM_BLD_free(bld); }
};
struct ParamFree {
    void operator()(OSSL_PARAM* params) const noexcept { OSSL_PARAM_free(params); }
};
struct PkeyCtxFree {
    void operator()(EVP_PKEY_CTX* ctx) const noexcept { EVP_PKEY_CTX_free(ctx); }
};
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxFree>;

// OpenSSL's name for the curve in its key parameters.
constexpr const char* kGroup = "P-256";
// SEC 1 section 2.3.3: the uncompressed form opens with this octet.
constexpr std::uint8_t kUncompressed = 0x04;

Pkey from_params(OSSL_PARAM_BLD* bld, int selection) noexcept {
    const std::unique_ptr<OSSL_PARAM, ParamFree> params{OSSL_PARAM_BLD_to_param(bld)};
    const PkeyCtx ctx{EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)};
    EVP_PKEY* raw = nullptr;
    if (!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1 ||
        EVP_PKEY_fromdata(ctx.get(), &raw, selection, params.get()) != 1) {
        return {};
    }
    return Pkey{raw};
}

} // namespace

Pkey import_public(std::span<const std::uint8_t> point) noexcept {
    if (point.size() != kPointBytes || point.front() != kUncompressed) {
        return {};
    }
    const std::unique_ptr<OSSL_PARAM_BLD, ParamBuildFree> bld{OSSL_PARAM_BLD_new()};
    if (!bld ||
        OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, kGroup, 0) != 1 ||
        OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(),
                                         point.size()) != 1) {
        return {};
    }
    Pkey key = from_params(bld.get(), EVP_PKEY_PUBLIC_KEY);
    if (!key) {
        return {};
    }
    // The import takes the point as it comes; the check is what puts it on the curve, without
    // which an ECDH with it could leak the private key bit by bit (an invalid-curve attack).
    const PkeyCtx check{EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr)};
    if (!check || EVP_PKEY_public_check(check.get()) != 1) {
        return {};
    }
    return key;
}

Pkey import_private(const Scalar& scalar) noexcept {
    const std::unique_ptr<EC_GROUP, GroupFree> group{
        EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1)};
    const Bignum priv{BN_bin2bn(scalar.data(), static_cast<int>(scalar.size()), nullptr)};
    if (!group || !priv || BN_is_zero(priv.get()) != 0 ||
        BN_cmp(priv.get(), EC_GROUP_get0_order(group.get())) >= 0) {
        return {};
    }
    // OpenSSL 3.0 imports a private key without deriving its public point, which the key's
    // users (ECDH's peer check, the VAPID k= parameter) need.
    const std::unique_ptr<EC_POINT, PointFree> pub{EC_POINT_new(group.get())};
    Point octets{};
    if (!pub || EC_POINT_mul(group.get(), pub.get(), priv.get(), nullptr, nullptr, nullptr) != 1 ||
        EC_POINT_point2oct(group.get(), pub.get(), POINT_CONVERSION_UNCOMPRESSED, octets.data(),
                           octets.size(), nullptr) != octets.size()) {
        return {};
    }
    const std::unique_ptr<OSSL_PARAM_BLD, ParamBuildFree> bld{OSSL_PARAM_BLD_new()};
    if (!bld ||
        OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, kGroup, 0) != 1 ||
        OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PRIV_KEY, priv.get()) != 1 ||
        OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, octets.data(),
                                         octets.size()) != 1) {
        return {};
    }
    return from_params(bld.get(), EVP_PKEY_KEYPAIR);
}

Pkey generate() noexcept {
    return Pkey{EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", kGroup)};
}

std::optional<Point> public_point(EVP_PKEY* key) noexcept {
    Point out{};
    std::size_t length = 0;
    if (key == nullptr ||
        EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, out.data(), out.size(),
                                        &length) != 1 ||
        length != out.size() || out.front() != kUncompressed) {
        return std::nullopt;
    }
    return out;
}

std::optional<Scalar> private_scalar(EVP_PKEY* key) noexcept {
    BIGNUM* raw = nullptr;
    if (key == nullptr || EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &raw) != 1) {
        return std::nullopt;
    }
    const Bignum bn{raw};
    Scalar out{};
    if (BN_bn2binpad(bn.get(), out.data(), static_cast<int>(out.size())) !=
        static_cast<int>(out.size())) {
        return std::nullopt;
    }
    return out;
}

std::optional<Scalar> ecdh(EVP_PKEY* mine, EVP_PKEY* peer) noexcept {
    const PkeyCtx ctx{EVP_PKEY_CTX_new_from_pkey(nullptr, mine, nullptr)};
    Scalar out{};
    std::size_t length = out.size();
    if (!ctx || peer == nullptr || EVP_PKEY_derive_init(ctx.get()) != 1 ||
        EVP_PKEY_derive_set_peer(ctx.get(), peer) != 1 ||
        EVP_PKEY_derive(ctx.get(), out.data(), &length) != 1 || length != out.size()) {
        return std::nullopt;
    }
    return out;
}

std::optional<Digest> hmac_sha256(std::span<const std::uint8_t> key,
                                  std::span<const std::uint8_t> data) noexcept {
    Digest out{};
    unsigned int length = 0;
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(), data.size(),
             out.data(), &length) == nullptr ||
        length != out.size()) {
        return std::nullopt;
    }
    return out;
}

} // namespace infra::webpush::detail
