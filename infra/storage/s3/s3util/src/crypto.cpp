#include "infra/s3util/crypto.hpp"

#include <cstddef>
#include <cstdlib>
#include <openssl/evp.h>
#include <span>
#include <string>
#include <string_view>

namespace infra::s3util {

namespace {

// Only an allocation failure or a provider configuration without SHA-256 gets here. Nothing
// can be signed after that, and carrying on would put unsigned requests on the wire.
[[noreturn]] void crypto_failure() noexcept {
    std::abort();
}

Sha256Digest digest(const void* data, std::size_t size) noexcept {
    Sha256Digest out{};
    std::size_t written = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, data, size, out.data(), &written) != 1 ||
        written != out.size()) {
        crypto_failure();
    }
    return out;
}

} // namespace

Sha256Digest sha256(std::span<const std::byte> data) noexcept {
    return digest(data.data(), data.size());
}

Sha256Digest sha256(std::string_view data) noexcept {
    return digest(data.data(), data.size());
}

Sha256Digest hmac_sha256(std::span<const unsigned char> key, std::string_view data) noexcept {
    // char and unsigned char may alias each other; OpenSSL just spells bytes the other way.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* const bytes = reinterpret_cast<const unsigned char*>(data.data());
    Sha256Digest out{};
    std::size_t written = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key.data(), key.size(), bytes,
                  data.size(), out.data(), out.size(), &written) == nullptr ||
        written != out.size()) {
        crypto_failure();
    }
    return out;
}

std::string to_hex(std::span<const unsigned char> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::size_t b : bytes) {
        out.push_back(kDigits[b >> 4U]);
        out.push_back(kDigits[b & 0xFU]);
    }
    return out;
}

} // namespace infra::s3util
