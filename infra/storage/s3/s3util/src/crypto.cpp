#include "infra/s3util/crypto.hpp"

#include <cstddef>
#include <cstdlib>
#include <memory>
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

class Sha256Stream::Context {
public:
    Context() : ctx_(EVP_MD_CTX_new()) {
        if (!ctx_ || EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr) != 1) {
            crypto_failure();
        }
    }

    [[nodiscard]] EVP_MD_CTX* get() const noexcept { return ctx_.get(); }

private:
    struct Free {
        void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
    };
    std::unique_ptr<EVP_MD_CTX, Free> ctx_;
};

Sha256Stream::Sha256Stream() : context_(std::make_unique<Context>()) {}
Sha256Stream::~Sha256Stream() = default;
Sha256Stream::Sha256Stream(Sha256Stream&&) noexcept = default;
Sha256Stream& Sha256Stream::operator=(Sha256Stream&&) noexcept = default;

void Sha256Stream::update(std::span<const std::byte> data) noexcept {
    if (EVP_DigestUpdate(context_->get(), data.data(), data.size()) != 1) {
        crypto_failure();
    }
}

Sha256Digest Sha256Stream::finish() noexcept {
    Sha256Digest out{};
    unsigned int written = 0;
    if (EVP_DigestFinal_ex(context_->get(), out.data(), &written) != 1 || written != out.size()) {
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
