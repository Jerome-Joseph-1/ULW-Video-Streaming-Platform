#pragma once

#include "core/ports/auth.hpp"
#include "core/util/time.hpp"
#include "infra/auth/claim_rules.hpp"

#include "jwk.hpp"
#include "jws.hpp"

#include <string_view>

namespace infra::auth::detail {

// RFC 7519 section 4.1.4 allows "a small leeway, usually no more than a few minutes". Hosts
// under NTP agree to milliseconds; a minute covers one whose sync has lapsed for a while
// without stretching a stolen token's life by much.
inline constexpr core::Seconds kClockSkew{60};

// `payload` must come from a token whose signature has already been checked.
[[nodiscard]] core::ports::VerifyResult check_claims(std::string_view payload,
                                                     const ClaimRules& rules, core::WallTime now);

// The signature first, and the claims only once it holds.
[[nodiscard]] core::ports::VerifyResult authenticate(const CompactJws& jws, const PublicKey& key,
                                                     const ClaimRules& rules, core::WallTime now);

} // namespace infra::auth::detail
