#pragma once

#include <initializer_list>
#include <memory>
#include <openssl/types.h>
#include <string>
#include <string_view>

namespace ulw::test {

// Freshly generated keys, so no test depends on a key pasted into the source.
class TestKey {
public:
    [[nodiscard]] static TestKey rsa(std::string kid, int bits = 2048);
    [[nodiscard]] static TestKey p256(std::string kid);
    [[nodiscard]] static TestKey ed25519(std::string kid);

    [[nodiscard]] const std::string& kid() const noexcept { return kid_; }
    // The public half as a JWK object. `extra` goes in verbatim after the key members, so a
    // test can add `,"alg":"PS256"` or break the object on purpose.
    [[nodiscard]] std::string jwk(std::string_view extra = {}) const;

private:
    struct Free {
        void operator()(EVP_PKEY* key) const noexcept;
    };

    TestKey(std::string kid, EVP_PKEY* key);

    std::string kid_;
    std::unique_ptr<EVP_PKEY, Free> key_;
};

// {"keys":[...]} around JWK objects given as text.
[[nodiscard]] std::string key_set(std::initializer_list<std::string_view> jwks);

} // namespace ulw::test
