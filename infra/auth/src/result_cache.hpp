#pragma once

#include "core/ports/auth.hpp"
#include "core/util/time.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace infra::auth::detail {

using TokenDigest = std::array<unsigned char, 32>;

// SHA-256. nullopt only if OpenSSL cannot compute it, which leaves the caller uncached.
[[nodiscard]] std::optional<TokenDigest> digest_token(std::string_view token) noexcept;

// Verdicts for tokens already verified, keyed by digest so a hit needs no parse and no
// signature check. Set-associative over storage allocated once: memory is fixed up front and
// a full set evicts its entry closest to expiry.
class ResultCache {
public:
    // One entry per client token seen in the last 15 minutes. 4096 entries of 224 bytes are
    // 0.9 MB of the gateway's 1 GB (ADR-0007); a client beyond that costs a signature check,
    // nothing more.
    static constexpr std::size_t kWays = 4;
    static constexpr std::size_t kSets = 1024;

    ResultCache();

    // A hit needs `now` before the entry's deadline and the token's own expiry still within
    // the skew; an entry failing either is dropped.
    [[nodiscard]] std::optional<core::ports::Claims>
    find(const TokenDigest& digest, core::MonoTime now, core::WallTime wall_now);
    void insert(const TokenDigest& digest, const core::ports::Claims& claims, core::MonoTime until);
    void clear() noexcept;

private:
    struct Entry {
        TokenDigest digest{};
        core::MonoTime until;
        std::optional<core::ports::Claims> claims;
    };

    [[nodiscard]] static std::size_t set_of(const TokenDigest& digest) noexcept;

    std::vector<Entry> entries_;
};

} // namespace infra::auth::detail
