#include "claims.hpp"

#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
#include "core/ports/auth.hpp"
#include "core/util/json.hpp"
#include "core/util/time.hpp"
#include "infra/auth/claim_rules.hpp"

#include "json_member.hpp"
#include "jwk.hpp"
#include "jws.hpp"
#include "signature.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::auth::detail {

using core::ports::AuthError;
using core::ports::Claims;
using core::ports::VerifyResult;

namespace {

// RFC 5321 section 4.5.3.1.3 caps a path at 256 octets including its angle brackets.
constexpr std::size_t kMaxEmailBytes = 254;

// A NumericDate the wall clock can hold. Anything beyond is malformed rather than clamped, so
// no arithmetic below can overflow the clock's nanosecond count.
std::expected<core::WallTime, AuthError> numeric_date(const core::json::Value& v) noexcept {
    constexpr auto kMin =
        std::chrono::duration_cast<core::Seconds>(core::WallTime::duration::min()).count() + 1;
    constexpr auto kMax =
        std::chrono::duration_cast<core::Seconds>(core::WallTime::duration::max()).count() - 1;
    const std::optional<std::int64_t> seconds = v.as_i64();
    if (!seconds || *seconds < kMin || *seconds > kMax) {
        return std::unexpected(AuthError::Malformed);
    }
    return core::WallTime{core::Seconds{*seconds}};
}

// RFC 7519 section 4.1.3: one string, or an array of which one must match.
std::expected<void, AuthError> check_audience(const core::json::Value& claims,
                                              std::string_view audience) {
    const core::json::Value* aud = claims.find("aud");
    if (aud == nullptr) {
        return std::unexpected(AuthError::WrongAudience);
    }
    if (const std::optional<std::string_view> one = aud->as_string()) {
        if (*one == audience) {
            return {};
        }
        return std::unexpected(AuthError::WrongAudience);
    }
    const std::vector<core::json::Value>* list = aud->as_array();
    if (list == nullptr || !std::ranges::all_of(*list, [](const core::json::Value& v) {
            return v.as_string().has_value();
        })) {
        return std::unexpected(AuthError::Malformed);
    }
    if (std::ranges::any_of(
            *list, [&](const core::json::Value& v) { return v.as_string() == audience; })) {
        return {};
    }
    return std::unexpected(AuthError::WrongAudience);
}

// The claim `subject_claim` names (`sub` by default). `sub` is a string (RFC 7519 section
// 4.1.2); a claim of the identity provider's own may also hold a non-negative integer, which is
// taken in its decimal form, since nothing pins down its JSON type.
std::expected<core::UserId, AuthError> subject_of(const core::json::Value& claims,
                                                  std::string_view subject_claim) {
    std::array<char, 20> digits{};
    std::string_view text;
    const core::json::Value* subject = claims.find(subject_claim);
    if (subject == nullptr) {
        return std::unexpected(AuthError::MissingSubject);
    }
    if (const std::optional<std::string_view> s = subject->as_string()) {
        text = *s;
    } else if (const std::optional<std::uint64_t> n = subject->as_u64();
               n && subject_claim != "sub") {
        // 20 digits hold any 64-bit value, so the conversion cannot run out of room.
        const std::to_chars_result r =
            std::to_chars(digits.data(), digits.data() + digits.size(), *n);
        text = {digits.data(), static_cast<std::size_t>(r.ptr - digits.data())};
    } else {
        return std::unexpected(AuthError::Malformed);
    }
    const auto user = core::UserId::parse(text);
    if (!user) {
        return std::unexpected(user.error() == core::DomainError::EmptyIdentifier
                                   ? AuthError::MissingSubject
                                   : AuthError::Malformed);
    }
    return *user;
}

std::expected<std::string, AuthError> email_of(const core::json::Value& claims) {
    const core::json::Value* email = claims.find("email");
    if (email == nullptr || email->is_null()) {
        return std::string{};
    }
    const std::optional<std::string_view> text = email->as_string();
    // A control character here would end up verbatim in a log line or a response header.
    if (!text || text->size() > kMaxEmailBytes || std::ranges::any_of(*text, [](char c) {
            return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
        })) {
        return std::unexpected(AuthError::Malformed);
    }
    return std::string(*text);
}

// The broadcaster rule, read leniently: a claim that is absent or of another shape is simply not
// a match, never a refusal of the token, which still serves everything else.
bool matches(const core::json::Value& doc, const ClaimRules& rules) {
    if (rules.broadcaster_claim.empty()) {
        return true;
    }
    const core::json::Value* claim = doc.find(rules.broadcaster_claim);
    if (claim == nullptr) {
        return false;
    }
    if (claim->as_string() == rules.broadcaster_value) {
        return true;
    }
    if (const auto flag = claim->as_bool()) {
        return *flag && rules.broadcaster_value == "true";
    }
    const auto* items = claim->as_array();
    return items != nullptr && std::ranges::any_of(*items, [&](const core::json::Value& item) {
               return item.as_string() == rules.broadcaster_value;
           });
}

} // namespace

VerifyResult check_claims(std::string_view payload, const ClaimRules& rules, core::WallTime now) {
    const auto doc = core::json::parse(payload);
    if (!doc || doc->as_object() == nullptr) {
        return std::unexpected(AuthError::Malformed);
    }
    if (string_member(*doc, "iss") != rules.issuer) {
        return std::unexpected(AuthError::WrongIssuer);
    }
    if (const auto aud = check_audience(*doc, rules.audience); !aud) {
        return std::unexpected(aud.error());
    }

    const core::json::Value* exp = doc->find("exp");
    if (exp == nullptr) {
        return std::unexpected(AuthError::Malformed);
    }
    const auto expires_at = numeric_date(*exp);
    if (!expires_at) {
        return std::unexpected(expires_at.error());
    }
    // `iat` is deliberately not read: the token contract (docs/integration/auth.md) asks for
    // exp and nbf only, and a second clock check would refuse tokens nbf already admits.
    // Written against `now` so that no addition can run past the clock's range.
    if (now - kClockSkew >= *expires_at) {
        return std::unexpected(AuthError::Expired);
    }
    if (const core::json::Value* nbf = doc->find("nbf")) {
        const auto not_before = numeric_date(*nbf);
        if (!not_before) {
            return std::unexpected(not_before.error());
        }
        if (now + kClockSkew < *not_before) {
            return std::unexpected(AuthError::NotYetValid);
        }
    }

    auto subject = subject_of(*doc, rules.subject_claim);
    if (!subject) {
        return std::unexpected(subject.error());
    }
    auto email = email_of(*doc);
    if (!email) {
        return std::unexpected(email.error());
    }
    return Claims{.subject = *subject,
                  .email = std::move(*email),
                  .expires_at = *expires_at,
                  .may_broadcast = matches(*doc, rules)};
}

VerifyResult authenticate(const CompactJws& jws, const PublicKey& key, const ClaimRules& rules,
                          core::WallTime now) {
    if (const auto signature = check_signature(jws, key); !signature) {
        return std::unexpected(signature.error());
    }
    return check_claims(jws.payload, rules, now);
}

} // namespace infra::auth::detail
