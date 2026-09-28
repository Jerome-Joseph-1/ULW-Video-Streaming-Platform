#include "result_cache.hpp"

#include "core/ports/auth.hpp"
#include "core/util/time.hpp"

#include "claims.hpp"

#include <algorithm>
#include <cstddef>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <optional>
#include <span>
#include <string_view>

namespace infra::auth::detail {

static_assert((ResultCache::kSets & (ResultCache::kSets - 1)) == 0, "set index is a mask");

std::optional<TokenDigest> digest_token(std::string_view token) noexcept {
    TokenDigest out{};
    std::size_t written = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, token.data(), token.size(), out.data(),
                     &written) != 1 ||
        written != out.size()) {
        ERR_clear_error();
        return std::nullopt;
    }
    return out;
}

ResultCache::ResultCache() : entries_(kSets * kWays) {}

std::size_t ResultCache::set_of(const TokenDigest& digest) noexcept {
    // The digest is uniform, so its first bytes are as good an index as any hash of them.
    const std::size_t index = (std::size_t{digest[0]} << 8U) | digest[1];
    return index & (kSets - 1);
}

std::optional<core::ports::Claims> ResultCache::find(const TokenDigest& digest, core::MonoTime now,
                                                     core::WallTime wall_now) {
    const std::span<Entry> set = std::span(entries_).subspan(set_of(digest) * kWays, kWays);
    for (Entry& e : set) {
        if (!e.claims || e.digest != digest) {
            continue;
        }
        if (now >= e.until || wall_now - kClockSkew >= e.claims->expires_at) {
            e.claims.reset();
            return std::nullopt;
        }
        return e.claims;
    }
    return std::nullopt;
}

void ResultCache::insert(const TokenDigest& digest, const core::ports::Claims& claims,
                         core::MonoTime until) {
    const std::span<Entry> set = std::span(entries_).subspan(set_of(digest) * kWays, kWays);
    // An empty way first, then the one due to go soonest.
    Entry& victim = *std::ranges::min_element(set, [](const Entry& a, const Entry& b) {
        if (a.claims.has_value() != b.claims.has_value()) {
            return !a.claims.has_value();
        }
        return a.until < b.until;
    });
    victim.digest = digest;
    victim.until = until;
    victim.claims = claims;
}

void ResultCache::clear() noexcept {
    for (Entry& e : entries_) {
        e.claims.reset();
    }
}

} // namespace infra::auth::detail
