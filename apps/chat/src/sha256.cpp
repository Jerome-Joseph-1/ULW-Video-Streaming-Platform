#include "sha256.hpp"

#include <openssl/evp.h>
#include <stdexcept>

namespace chat {

std::array<unsigned char, 32> sha256(std::string_view text) {
    std::array<unsigned char, 32> digest{};
    std::size_t length = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, text.data(), text.size(), digest.data(),
                     &length) != 1 ||
        length != digest.size()) {
        // SHA-256 is compiled into every OpenSSL this builds against; only a broken library
        // gets here.
        throw std::runtime_error("SHA-256 unavailable");
    }
    return digest;
}

} // namespace chat
