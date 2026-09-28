#pragma once

#include "core/ports/auth.hpp"

#include "jwk.hpp"
#include "jws.hpp"

#include <expected>

namespace infra::auth::detail {

// UnsupportedAlgorithm when the key does not allow the header's algorithm; BadSignature for
// every other failure, OpenSSL's included.
[[nodiscard]] std::expected<void, core::ports::AuthError> check_signature(const CompactJws& jws,
                                                                          const PublicKey& key);

} // namespace infra::auth::detail
