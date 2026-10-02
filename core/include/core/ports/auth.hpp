#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace core::ports {

// How far past its `exp` a token is still accepted, for clocks that disagree a little
// (infra/auth's claim check says why a minute).
inline constexpr Seconds kTokenClockSkew{60};

struct Claims {
    UserId subject;
    std::string email;
    WallTime expires_at;
};

enum class AuthError : std::uint8_t {
    Malformed,
    UnsupportedAlgorithm,
    UnknownKey,
    BadSignature,
    Expired,
    NotYetValid,
    WrongIssuer,
    WrongAudience,
    MissingSubject,
    // The key set could not be fetched and no cached key fits.
    KeysUnavailable,
};

[[nodiscard]] std::string_view to_string(AuthError e) noexcept;

using VerifyResult = std::expected<Claims, AuthError>;

class IKeyWaiter {
public:
    virtual ~IKeyWaiter() = default;
    virtual void on_keys_refreshed() noexcept = 0;
};

class IJwtVerifier {
public:
    virtual ~IJwtVerifier() = default;
    // Answers at once for any token whose key is known. For a key it has not seen, it returns
    // nullopt, starts a refresh of the key set unless one is running, and calls `waiter` on the
    // reactor thread when that refresh ends; the caller then verifies again. A key still
    // missing after a refresh is answered with UnknownKey, so one unseen key costs one fetch.
    [[nodiscard]] virtual std::optional<VerifyResult> verify(std::string_view token, WallTime now,
                                                             IKeyWaiter& waiter) = 0;
    // For a waiter about to be destroyed before its refresh ends.
    virtual void cancel_wait(IKeyWaiter& waiter) noexcept = 0;
    // Whether the keys it had were dropped for going too long without a successful refresh,
    // so that every token is refused until one succeeds. A fixed key set never is.
    [[nodiscard]] virtual bool keys_expired() const noexcept { return false; }
    // Forgets the cached key set and every verified token it remembers, and starts a refresh at
    // once (on SIGHUP, after the issuer rotated its key: ADR-0079). Tokens then wait on that
    // refresh as for an unseen key. On the reactor thread; never blocks. A fixed key set has
    // nothing to forget.
    virtual void drop_caches() noexcept {}
};

} // namespace core::ports
