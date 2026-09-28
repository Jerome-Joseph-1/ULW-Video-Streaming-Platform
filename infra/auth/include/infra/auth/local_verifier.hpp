#pragma once

#include "core/ports/auth.hpp"
#include "core/util/time.hpp"
#include "infra/auth/claim_rules.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

namespace infra::auth {

namespace detail {
struct KeySet;
} // namespace detail

enum class KeySetError : std::uint8_t {
    // Not a JWK set document.
    Malformed,
    // A member that is not an Ed25519 signing key with a kid of its own.
    UnusableKey,
    Empty,
};

[[nodiscard]] std::string_view to_string(KeySetError e) noexcept;

// Offline development: a fixed JWK set of Ed25519 keys (ulw_devtoken makes them) checked
// against the same claim rules as production. Nothing is ever fetched, so every answer is
// immediate and no waiter is ever held.
class Ed25519LocalVerifier final : public core::ports::IJwtVerifier {
public:
    [[nodiscard]] static std::expected<Ed25519LocalVerifier, KeySetError>
    create(std::string_view jwks, ClaimRules rules);

    Ed25519LocalVerifier(Ed25519LocalVerifier&& other) noexcept;
    Ed25519LocalVerifier& operator=(Ed25519LocalVerifier&& other) noexcept;
    Ed25519LocalVerifier(const Ed25519LocalVerifier&) = delete;
    Ed25519LocalVerifier& operator=(const Ed25519LocalVerifier&) = delete;
    ~Ed25519LocalVerifier() override;

    [[nodiscard]] std::optional<core::ports::VerifyResult>
    verify(std::string_view token, core::WallTime now, core::ports::IKeyWaiter& waiter) override;
    void cancel_wait(core::ports::IKeyWaiter& waiter) noexcept override;

private:
    Ed25519LocalVerifier(std::unique_ptr<detail::KeySet> keys, ClaimRules rules) noexcept;

    std::unique_ptr<detail::KeySet> keys_;
    ClaimRules rules_;
};

} // namespace infra::auth
