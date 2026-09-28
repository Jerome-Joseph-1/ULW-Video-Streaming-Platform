#pragma once

#include "jws.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <openssl/types.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace infra::auth::detail {

struct PkeyFree {
    void operator()(EVP_PKEY* key) const noexcept;
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;

enum class KeyType : std::uint8_t { Rsa, P256, Ed25519 };

struct PublicKey {
    std::string kid;
    KeyType type;
    // Set when the JWK names its algorithm; the token may then use that one only.
    std::optional<Algorithm> alg;
    Pkey pkey;
};

// The algorithm is the key's to choose: the header only picks among what the key allows.
[[nodiscard]] bool key_allows(const PublicKey& key, Algorithm alg) noexcept;

// Same key material and the same algorithms allowed.
[[nodiscard]] bool same_key(const PublicKey& a, const PublicKey& b) noexcept;

struct KeySet {
    std::vector<PublicKey> keys;
    // Members of the document this service cannot use: another key type or curve, an
    // encryption key, an RSA modulus under 2048 bits, no kid, or a kid seen earlier.
    std::size_t skipped = 0;

    [[nodiscard]] const PublicKey* find(std::string_view kid) const noexcept;
};

// RFC 7517 section 5. nullopt only when the document is not a JWK set at all; a key that
// cannot be used is counted in `skipped` rather than failing the whole set.
[[nodiscard]] std::optional<KeySet> parse_key_set(std::string_view json);

} // namespace infra::auth::detail
