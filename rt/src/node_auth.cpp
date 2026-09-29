#include "node_auth.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <string_view>
#include <vector>

namespace rt::auth {

namespace {

std::optional<wire::Mac> tag(std::string_view role, std::span<const std::byte> secret,
                             const core::NodeId& dialer, const core::NodeId& acceptor,
                             const wire::Nonce& dialer_nonce, const wire::Nonce& acceptor_nonce) {
    // Every field but the nonces is length-prefixed, so no two transcripts encode alike.
    std::vector<unsigned char> message;
    const auto put = [&](std::span<const std::byte> bytes) {
        for (const std::byte b : bytes) {
            message.push_back(std::to_integer<unsigned char>(b));
        }
    };
    const auto put_text = [&](std::string_view text) {
        message.push_back(static_cast<unsigned char>(text.size()));
        put(std::as_bytes(std::span{text}));
    };
    put_text("ulw-node-channel-v1");
    put_text(role);
    put_text(dialer.view());
    put_text(acceptor.view());
    put(dialer_nonce);
    put(acceptor_nonce);

    wire::Mac out{};
    std::size_t written = 0;
    const auto* key = std::to_address(secret.begin());
    // OpenSSL takes the key and output as untyped bytes.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr,
                  reinterpret_cast<const unsigned char*>(key), secret.size(), message.data(),
                  message.size(), reinterpret_cast<unsigned char*>(out.data()), out.size(),
                  &written) == nullptr ||
        written != out.size()) {
        return std::nullopt;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return out;
}

} // namespace

std::optional<wire::Mac> acceptor_tag(std::span<const std::byte> secret, const core::NodeId& dialer,
                                      const core::NodeId& acceptor, const wire::Nonce& dialer_nonce,
                                      const wire::Nonce& acceptor_nonce) {
    return tag("acceptor", secret, dialer, acceptor, dialer_nonce, acceptor_nonce);
}

std::optional<wire::Mac> dialer_tag(std::span<const std::byte> secret, const core::NodeId& dialer,
                                    const core::NodeId& acceptor, const wire::Nonce& dialer_nonce,
                                    const wire::Nonce& acceptor_nonce) {
    return tag("dialer", secret, dialer, acceptor, dialer_nonce, acceptor_nonce);
}

bool same_tag(const wire::Mac& a, const wire::Mac& b) noexcept {
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace rt::auth
