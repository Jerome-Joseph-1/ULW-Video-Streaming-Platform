#include "infra/auth/local_verifier.hpp"

#include "core/ports/auth.hpp"
#include "core/util/time.hpp"
#include "infra/auth/claim_rules.hpp"

#include "claims.hpp"
#include "jwk.hpp"
#include "jws.hpp"

#include <algorithm>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace infra::auth {

using core::ports::AuthError;
using core::ports::VerifyResult;

std::string_view to_string(KeySetError e) noexcept {
    switch (e) {
    case KeySetError::Malformed:
        return "not a JWK set";
    case KeySetError::UnusableKey:
        return "key set holds a key other than an Ed25519 signing key with a kid";
    case KeySetError::Empty:
        return "key set holds no keys";
    }
    return "unknown key set error";
}

std::expected<Ed25519LocalVerifier, KeySetError> Ed25519LocalVerifier::create(std::string_view jwks,
                                                                              ClaimRules rules) {
    std::optional<detail::KeySet> set = detail::parse_key_set(jwks);
    if (!set) {
        return std::unexpected(KeySetError::Malformed);
    }
    // A development key set is written by hand or by ulw_devtoken; a key it cannot use is a
    // mistake to report, not a member to skip as a published set's would be.
    if (set->skipped != 0 || !std::ranges::all_of(set->keys, [](const detail::PublicKey& k) {
            return k.type == detail::KeyType::Ed25519;
        })) {
        return std::unexpected(KeySetError::UnusableKey);
    }
    if (set->keys.empty()) {
        return std::unexpected(KeySetError::Empty);
    }
    return Ed25519LocalVerifier{std::make_unique<detail::KeySet>(std::move(*set)),
                                std::move(rules)};
}

Ed25519LocalVerifier::Ed25519LocalVerifier(std::unique_ptr<detail::KeySet> keys,
                                           ClaimRules rules) noexcept
    : keys_(std::move(keys)), rules_(std::move(rules)) {}

Ed25519LocalVerifier::Ed25519LocalVerifier(Ed25519LocalVerifier&& other) noexcept = default;
Ed25519LocalVerifier&
Ed25519LocalVerifier::operator=(Ed25519LocalVerifier&& other) noexcept = default;
Ed25519LocalVerifier::~Ed25519LocalVerifier() = default;

std::optional<VerifyResult> Ed25519LocalVerifier::verify(std::string_view token, core::WallTime now,
                                                         core::ports::IKeyWaiter& /*waiter*/) {
    const auto jws = detail::parse_compact(token);
    if (!jws) {
        return VerifyResult{std::unexpected(jws.error())};
    }
    const detail::PublicKey* key = keys_->find(jws->kid);
    if (key == nullptr) {
        return VerifyResult{std::unexpected(AuthError::UnknownKey)};
    }
    return detail::authenticate(*jws, *key, rules_, now);
}

void Ed25519LocalVerifier::cancel_wait(core::ports::IKeyWaiter& /*waiter*/) noexcept {}

} // namespace infra::auth
