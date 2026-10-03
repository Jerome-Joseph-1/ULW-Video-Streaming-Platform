#include "infra/webpush/ece.hpp"

#include "p256.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string_view>

namespace infra::webpush {

namespace {

using detail::Pkey;

constexpr std::size_t kKeyBytes = 16;
constexpr std::size_t kNonceBytes = 12;
// RFC 8188 section 2: the delimiter of the last record, after which only zeros may follow.
constexpr std::uint8_t kLastRecord = 0x02;

struct CipherCtxFree {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept { EVP_CIPHER_CTX_free(ctx); }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxFree>;

// What the key schedule yields, wiped when it goes.
struct Keys {
    std::array<std::uint8_t, kKeyBytes> cek{};
    std::array<std::uint8_t, kNonceBytes> nonce{};

    Keys() = default;
    Keys(const Keys&) = delete;
    Keys& operator=(const Keys&) = delete;
    ~Keys() {
        OPENSSL_cleanse(cek.data(), cek.size());
        OPENSSL_cleanse(nonce.data(), nonce.size());
    }
};

std::span<const std::uint8_t> bytes_of(std::string_view text) noexcept {
    // char and unsigned char may alias each other.
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// RFC 8291 section 3.4 and RFC 8188 section 2.2: HKDF-SHA-256 written out as HMACs, since
// every output is one block or less (HKDF-Expand of length <= 32 is HMAC(PRK, info || 0x01)).
bool derive(const detail::Scalar& ecdh_secret, const AuthSecret& auth, const PublicKey& ua_public,
            const PublicKey& as_public, const Salt& salt, Keys& out) noexcept {
    constexpr std::string_view kKeyInfo{"WebPush: info\0", 14};
    constexpr std::string_view kCekInfo{"Content-Encoding: aes128gcm\0\x01", 29};
    constexpr std::string_view kNonceInfo{"Content-Encoding: nonce\0\x01", 25};
    // PRK_key = HKDF-Extract(salt = auth_secret, IKM = ecdh_secret)
    auto prk_key = detail::hmac_sha256(auth, ecdh_secret);
    // IKM = HKDF-Expand(PRK_key, key_info, 32), key_info naming both public keys.
    std::array<std::uint8_t, kKeyInfo.size() + (2 * kPublicKeyBytes) + 1> key_info{};
    auto* at = std::ranges::copy(bytes_of(kKeyInfo), key_info.begin()).out;
    at = std::ranges::copy(ua_public, at).out;
    at = std::ranges::copy(as_public, at).out;
    *at = 0x01;
    std::optional<detail::Digest> ikm;
    if (prk_key) {
        ikm = detail::hmac_sha256(*prk_key, key_info);
        OPENSSL_cleanse(prk_key->data(), prk_key->size());
    }
    // PRK = HKDF-Extract(salt, IKM); CEK and NONCE expand it.
    std::optional<detail::Digest> prk;
    if (ikm) {
        prk = detail::hmac_sha256(salt, *ikm);
        OPENSSL_cleanse(ikm->data(), ikm->size());
    }
    if (!prk) {
        return false;
    }
    auto cek = detail::hmac_sha256(*prk, bytes_of(kCekInfo));
    auto nonce = detail::hmac_sha256(*prk, bytes_of(kNonceInfo));
    OPENSSL_cleanse(prk->data(), prk->size());
    const bool ok = cek && nonce;
    if (ok) {
        std::copy_n(cek->begin(), out.cek.size(), out.cek.begin());
        std::copy_n(nonce->begin(), out.nonce.size(), out.nonce.begin());
    }
    if (cek) {
        OPENSSL_cleanse(cek->data(), cek->size());
    }
    if (nonce) {
        OPENSSL_cleanse(nonce->data(), nonce->size());
    }
    return ok;
}

std::expected<std::vector<std::uint8_t>, EceError> seal(EVP_PKEY* sender, const Salt& salt,
                                                        const PublicKey& ua_public,
                                                        const AuthSecret& auth,
                                                        std::span<const std::uint8_t> plaintext) {
    if (plaintext.size() > kMaxPlaintext) {
        return std::unexpected(EceError::TooLarge);
    }
    const Pkey peer = detail::import_public(ua_public);
    if (!peer) {
        return std::unexpected(EceError::BadKey);
    }
    const auto as_public = detail::public_point(sender);
    auto secret = detail::ecdh(sender, peer.get());
    if (!as_public || !secret) {
        return std::unexpected(EceError::Crypto);
    }
    Keys keys;
    const bool derived = derive(*secret, auth, ua_public, *as_public, salt, keys);
    OPENSSL_cleanse(secret->data(), secret->size());
    if (!derived) {
        return std::unexpected(EceError::Crypto);
    }

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderBytes + plaintext.size() + 1 + kTagBytes);
    out.insert(out.end(), salt.begin(), salt.end());
    for (const unsigned shift : {24U, 16U, 8U, 0U}) {
        out.push_back(static_cast<std::uint8_t>((kRecordSize >> shift) & 0xffU));
    }
    out.push_back(static_cast<std::uint8_t>(kPublicKeyBytes));
    out.insert(out.end(), as_public->begin(), as_public->end());

    const CipherCtx ctx{EVP_CIPHER_CTX_new()};
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, keys.cek.data(),
                                   keys.nonce.data()) != 1) {
        return std::unexpected(EceError::Crypto);
    }
    const std::size_t start = out.size();
    out.resize(start + plaintext.size() + 1 + kTagBytes);
    int written = 0;
    int total = 0;
    if (EVP_EncryptUpdate(ctx.get(), out.data() + start, &written, plaintext.data(),
                          static_cast<int>(plaintext.size())) != 1) {
        return std::unexpected(EceError::Crypto);
    }
    total += written;
    if (EVP_EncryptUpdate(ctx.get(), out.data() + start + total, &written, &kLastRecord, 1) != 1) {
        return std::unexpected(EceError::Crypto);
    }
    total += written;
    if (EVP_EncryptFinal_ex(ctx.get(), out.data() + start + total, &written) != 1) {
        return std::unexpected(EceError::Crypto);
    }
    total += written;
    if (static_cast<std::size_t>(total) != plaintext.size() + 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagBytes),
                            out.data() + start + total) != 1) {
        return std::unexpected(EceError::Crypto);
    }
    return out;
}

} // namespace

bool is_p256_point(std::span<const std::uint8_t> bytes) noexcept {
    return static_cast<bool>(detail::import_public(bytes));
}

std::expected<KeyPair, EceError> generate_key_pair() noexcept {
    const Pkey key = detail::generate();
    const auto pub = detail::public_point(key.get());
    const auto priv = detail::private_scalar(key.get());
    if (!pub || !priv) {
        return std::unexpected(EceError::Crypto);
    }
    return KeyPair{.private_key = *priv, .public_key = *pub};
}

std::expected<std::vector<std::uint8_t>, EceError>
encrypt(const PublicKey& ua_public, const AuthSecret& auth,
        std::span<const std::uint8_t> plaintext) noexcept {
    try {
        const Pkey sender = detail::generate();
        Salt salt{};
        if (!sender || RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
            return std::unexpected(EceError::Crypto);
        }
        return seal(sender.get(), salt, ua_public, auth, plaintext);
    } catch (const std::bad_alloc&) {
        return std::unexpected(EceError::Crypto);
    }
}

std::expected<std::vector<std::uint8_t>, EceError>
encrypt_with(const PrivateKey& as_private, const Salt& salt, const PublicKey& ua_public,
             const AuthSecret& auth, std::span<const std::uint8_t> plaintext) noexcept {
    try {
        const Pkey sender = detail::import_private(as_private);
        if (!sender) {
            return std::unexpected(EceError::BadKey);
        }
        return seal(sender.get(), salt, ua_public, auth, plaintext);
    } catch (const std::bad_alloc&) {
        return std::unexpected(EceError::Crypto);
    }
}

std::expected<std::vector<std::uint8_t>, EceError>
decrypt(const PrivateKey& ua_private, const AuthSecret& auth,
        std::span<const std::uint8_t> message) noexcept {
    try {
        if (message.size() < kHeaderBytes + 1 + kTagBytes) {
            return std::unexpected(EceError::Malformed);
        }
        Salt salt{};
        std::copy_n(message.begin(), salt.size(), salt.begin());
        std::uint32_t rs = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            rs = (rs << 8U) | message[kSaltBytes + i];
        }
        const std::span<const std::uint8_t> keyid =
            message.subspan(kSaltBytes + 5, kPublicKeyBytes);
        const std::span<const std::uint8_t> sealed = message.subspan(kHeaderBytes);
        // One record: the whole of what follows the header, no larger than rs says a record is.
        if (message[kSaltBytes + 4] != kPublicKeyBytes || sealed.size() > rs) {
            return std::unexpected(EceError::Malformed);
        }
        const Pkey me = detail::import_private(ua_private);
        const Pkey sender = detail::import_public(keyid);
        if (!me || !sender) {
            return std::unexpected(EceError::BadKey);
        }
        const auto ua_public = detail::public_point(me.get());
        auto secret = detail::ecdh(me.get(), sender.get());
        if (!ua_public || !secret) {
            return std::unexpected(EceError::Crypto);
        }
        PublicKey as_public{};
        std::ranges::copy(keyid, as_public.begin());
        Keys keys;
        const bool derived = derive(*secret, auth, *ua_public, as_public, salt, keys);
        OPENSSL_cleanse(secret->data(), secret->size());
        if (!derived) {
            return std::unexpected(EceError::Crypto);
        }
        const std::span<const std::uint8_t> ciphertext = sealed.first(sealed.size() - kTagBytes);
        std::array<std::uint8_t, kTagBytes> tag{};
        std::ranges::copy(sealed.last(kTagBytes), tag.begin());
        const CipherCtx ctx{EVP_CIPHER_CTX_new()};
        std::vector<std::uint8_t> out(ciphertext.size());
        int written = 0;
        int final_written = 0;
        if (!ctx ||
            EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, keys.cek.data(),
                               keys.nonce.data()) != 1 ||
            EVP_DecryptUpdate(ctx.get(), out.data(), &written, ciphertext.data(),
                              static_cast<int>(ciphertext.size())) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()),
                                tag.data()) != 1 ||
            EVP_DecryptFinal_ex(ctx.get(), out.data() + written, &final_written) != 1) {
            return std::unexpected(EceError::Malformed);
        }
        out.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(final_written));
        // Padding: zeros after the delimiter, read from the end.
        while (!out.empty() && out.back() == 0) {
            out.pop_back();
        }
        if (out.empty() || out.back() != kLastRecord) {
            return std::unexpected(EceError::Malformed);
        }
        out.pop_back();
        return out;
    } catch (const std::bad_alloc&) {
        return std::unexpected(EceError::Crypto);
    }
}

} // namespace infra::webpush
