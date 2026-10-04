#pragma once

#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace devtoken {

enum class DevKeyError : std::uint8_t {
    // Not an Ed25519 private JWK.
    Malformed,
    // The file's "x" is not the public half of its "d".
    Mismatch,
    // OpenSSL could not generate, load or use the key.
    Crypto,
};

[[nodiscard]] std::string_view to_string(DevKeyError e) noexcept;

// A week covers a test environment left running over a weekend; a longer-lived token is one
// that ends up pasted somewhere it outlives its purpose.
inline constexpr core::Seconds kMaxTtl{std::int64_t{7} * 24 * 3600};

// Whole seconds from 1 to kMaxTtl, nothing else.
[[nodiscard]] std::optional<core::Seconds> parse_ttl(std::string_view text) noexcept;

struct MintRequest {
    std::string issuer;
    std::string audience;
    std::string subject;
    // Left out of the token when empty.
    std::string email;
    core::Seconds ttl{};
    // OAuth's `scope`, space-separated, as an identity provider's client-credentials grant
    // gives a backend (the service APIs, ULW_SERVICE_SCOPE): left out when empty.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::string scope{};
};

// An Ed25519 signing key for local runs, kept on disk as a private JWK. It signs tokens only
// the local verifier accepts, and the file holds it in the clear anyway, so no effort goes into
// scrubbing copies from memory. Its kid is the RFC 7638 thumbprint, so the same key always
// carries the same kid.
class DevKey {
public:
    [[nodiscard]] static std::expected<DevKey, DevKeyError> generate();
    [[nodiscard]] static std::expected<DevKey, DevKeyError> from_private_jwk(std::string_view json);

    DevKey(DevKey&&) noexcept;
    DevKey& operator=(DevKey&&) noexcept;
    DevKey(const DevKey&) = delete;
    DevKey& operator=(const DevKey&) = delete;
    ~DevKey();

    [[nodiscard]] const std::string& kid() const noexcept { return kid_; }
    // What keygen writes to the key file.
    [[nodiscard]] std::expected<std::string, DevKeyError> private_jwk() const;
    // The {"keys":[...]} document the local verifier loads.
    [[nodiscard]] std::string public_jwks() const;
    [[nodiscard]] std::expected<std::string, DevKeyError> mint(const MintRequest& request,
                                                               core::WallTime now) const;

private:
    struct Pkey;

    [[nodiscard]] static std::expected<DevKey, DevKeyError> adopt(std::unique_ptr<Pkey> key);
    DevKey(std::unique_ptr<Pkey> key, std::string x, std::string kid) noexcept;

    std::unique_ptr<Pkey> key_;
    // Public key, base64url.
    std::string x_;
    std::string kid_;
};

} // namespace devtoken
