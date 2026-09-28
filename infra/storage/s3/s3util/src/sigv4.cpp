#include "infra/s3util/sigv4.hpp"

#include "core/util/time.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/crypto.hpp"
#include "infra/s3util/url.hpp"

#include "signing_key.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <openssl/crypto.h>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::s3util {

namespace {

constexpr std::string_view kAlgorithm = "AWS4-HMAC-SHA256";
// SigV4 refuses X-Amz-Expires beyond seven days.
constexpr core::Seconds kMaxPresignExpiry{604'800};

void wipe(Sha256Digest& key) noexcept {
    OPENSSL_cleanse(key.data(), key.size());
}

void put_digits(std::span<char> out, std::uint64_t value) noexcept {
    for (char& digit : out | std::views::reverse) {
        digit = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
}

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool is_blank(char c) noexcept {
    return c == ' ' || c == '\t';
}

// SigV4's Trimall: no leading or trailing blanks, and each inner run of them becomes one space.
std::string trim_all(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    bool pending_space = false;
    for (const char c : value) {
        if (is_blank(c)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(c);
    }
    return out;
}

// Repeated names are folded into one comma-joined header, which is what S3 signs for them.
std::vector<Header> normalize(std::span<const Header> headers) {
    std::vector<Header> lowered;
    lowered.reserve(headers.size());
    for (const auto& h : headers) {
        std::string name(h.name);
        std::ranges::transform(name, name.begin(), ascii_lower);
        lowered.push_back({.name = std::move(name), .value = trim_all(h.value)});
    }
    std::ranges::stable_sort(lowered, {}, &Header::name);

    std::vector<Header> merged;
    merged.reserve(lowered.size());
    for (auto& h : lowered) {
        if (!merged.empty() && merged.back().name == h.name) {
            merged.back().value.append(1, ',').append(h.value);
        } else {
            merged.push_back(std::move(h));
        }
    }
    return merged;
}

std::string signed_headers(std::span<const Header> normalized) {
    std::string out;
    for (const auto& h : normalized) {
        if (!out.empty()) {
            out.push_back(';');
        }
        out.append(h.name);
    }
    return out;
}

std::string canonical_text(std::string_view method, const RequestTarget& target,
                           std::span<const Header> normalized, std::string_view payload_hash) {
    std::string text;
    text.append(method).append(1, '\n');
    text.append(uri_encode_path(target.path)).append(1, '\n');
    text.append(canonical_query(target.query)).append(1, '\n');
    // Each header line carries its own newline and the block is then terminated by another,
    // so an empty line always separates it from the signed-header list.
    for (const auto& h : normalized) {
        text.append(h.name).append(1, ':').append(h.value).append(1, '\n');
    }
    text.append(1, '\n');
    text.append(signed_headers(normalized)).append(1, '\n');
    text.append(payload_hash);
    return text;
}

std::string credential_scope(const AmzDate& date, std::string_view region) {
    std::string scope(date.date());
    scope.append(1, '/').append(region).append("/s3/aws4_request");
    return scope;
}

} // namespace

std::string payload_sha256(std::span<const std::byte> body) {
    return to_hex(sha256(body));
}

AmzDate::AmzDate(core::WallTime t) noexcept {
    using std::chrono::floor;
    const auto second = floor<std::chrono::seconds>(t);
    const std::chrono::sys_days day = floor<std::chrono::sys_days::duration>(second);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss hms{second - day};
    const std::span<char> out(text_);
    // system_clock spans 1678..2262 in nanoseconds, so the year is always four digits.
    put_digits(out.subspan(0, 4), static_cast<std::uint64_t>(static_cast<int>(ymd.year())));
    put_digits(out.subspan(4, 2), static_cast<unsigned>(ymd.month()));
    put_digits(out.subspan(6, 2), static_cast<unsigned>(ymd.day()));
    out[8] = 'T';
    put_digits(out.subspan(9, 2), static_cast<std::uint64_t>(hms.hours().count()));
    put_digits(out.subspan(11, 2), static_cast<std::uint64_t>(hms.minutes().count()));
    put_digits(out.subspan(13, 2), static_cast<std::uint64_t>(hms.seconds().count()));
    out[15] = 'Z';
}

std::string canonical_request(std::string_view method, const RequestTarget& target,
                              std::span<const Header> headers, std::string_view payload_hash) {
    return canonical_text(method, target, normalize(headers), payload_hash);
}

Sha256Digest derive_signing_key(const SecretString& secret, const AmzDate& date,
                                std::string_view region) {
    constexpr std::string_view kPrefix = "AWS4";
    std::vector<unsigned char> seed;
    // Reserved up front so no reallocation leaves a copy of the secret behind.
    seed.reserve(kPrefix.size() + secret.reveal().size());
    for (const char c : kPrefix) {
        seed.push_back(static_cast<unsigned char>(c));
    }
    for (const char c : secret.reveal()) {
        seed.push_back(static_cast<unsigned char>(c));
    }
    Sha256Digest k_date = hmac_sha256(seed, date.date());
    OPENSSL_cleanse(seed.data(), seed.size());
    Sha256Digest k_region = hmac_sha256(k_date, region);
    Sha256Digest k_service = hmac_sha256(k_region, "s3");
    const Sha256Digest k_signing = hmac_sha256(k_service, "aws4_request");
    wipe(k_date);
    wipe(k_region);
    wipe(k_service);
    return k_signing;
}

// A signing key depends only on the date and the credentials (the region is fixed per
// signer), so deriving it four HMACs deep for every request is waste. The synchronous client
// signs from a small thread pool that shares one signer; a mutex around a handful of entries
// costs nothing next to the request being signed, and unlike per-thread caches it derives each
// key once. The secret is part of the key because an access key id can outlive a rotation.
class Signer::KeyCache {
public:
    KeyCache() = default;
    KeyCache(const KeyCache&) = delete;
    KeyCache& operator=(const KeyCache&) = delete;
    KeyCache(KeyCache&&) = delete;
    KeyCache& operator=(KeyCache&&) = delete;
    ~KeyCache() = default;

    [[nodiscard]] Sha256Digest get(const AmzDate& date, std::string_view region,
                                   const Credentials& credentials) {
        const std::scoped_lock lock(mutex_);
        const auto hit = std::ranges::find_if(entries_, [&](const Entry& e) {
            return e.date == date.date() && e.access_key_id == credentials.access_key_id() &&
                   e.secret.reveal() == credentials.secret_access_key().reveal();
        });
        if (hit != entries_.end()) {
            return hit->key.bytes();
        }
        Entry fresh{.date = std::string(date.date()),
                    .access_key_id = std::string(credentials.access_key_id()),
                    .secret = credentials.secret_access_key(),
                    .key = detail::SigningKey{
                        derive_signing_key(credentials.secret_access_key(), date, region)}};
        if (entries_.size() < kMaxEntries) {
            entries_.push_back(std::move(fresh));
            return entries_.back().key.bytes();
        }
        // Overwritten in place, so the evicted key never survives in a vacated slot.
        auto& oldest = *std::ranges::min_element(entries_, {}, &Entry::date);
        oldest = std::move(fresh);
        return oldest.key.bytes();
    }

private:
    // Two dates straddling midnight, times old and new credentials during a rotation.
    static constexpr std::size_t kMaxEntries = 4;

    // Moving an entry, into the vector or over the oldest one, wipes the key it moved from.
    struct Entry {
        std::string date;
        std::string access_key_id;
        SecretString secret;
        detail::SigningKey key;
    };

    std::mutex mutex_;
    std::vector<Entry> entries_;
};

Signer::Signer(std::string region)
    : region_(std::move(region)), cache_(std::make_unique<KeyCache>()) {}

Signer::Signer(Signer&&) noexcept = default;
Signer& Signer::operator=(Signer&&) noexcept = default;
Signer::~Signer() = default;

std::string Signer::signature(const AmzDate& date, std::string_view canonical,
                              const Credentials& credentials) const {
    std::string to_sign(kAlgorithm);
    to_sign.append(1, '\n').append(date.datetime()).append(1, '\n');
    to_sign.append(credential_scope(date, region_)).append(1, '\n');
    to_sign.append(to_hex(sha256(canonical)));
    Sha256Digest key = cache_->get(date, region_, credentials);
    std::string result = to_hex(hmac_sha256(key, to_sign));
    wipe(key);
    return result;
}

std::vector<Header> Signer::sign(std::string_view method, const RequestTarget& target,
                                 std::span<const Header> headers, std::string_view payload_hash,
                                 const Credentials& credentials, core::WallTime now) const {
    const AmzDate date(now);
    std::vector<Header> all(headers.begin(), headers.end());
    all.push_back({.name = "host", .value = target.host});
    all.push_back({.name = "x-amz-content-sha256", .value = std::string(payload_hash)});
    all.push_back({.name = "x-amz-date", .value = std::string(date.datetime())});
    if (const auto& token = credentials.session_token()) {
        all.push_back({.name = "x-amz-security-token", .value = std::string(token->reveal())});
    }
    all = normalize(all);

    const std::string signature_hex =
        signature(date, canonical_text(method, target, all, payload_hash), credentials);
    std::string authorization(kAlgorithm);
    authorization.append(" Credential=").append(credentials.access_key_id()).append(1, '/');
    authorization.append(credential_scope(date, region_));
    authorization.append(", SignedHeaders=").append(signed_headers(all));
    authorization.append(", Signature=").append(signature_hex);
    all.push_back({.name = "authorization", .value = std::move(authorization)});
    return all;
}

std::expected<std::string, PresignError>
Signer::presign(std::string_view method, const RequestTarget& target, core::Seconds expires,
                const Credentials& credentials, core::WallTime now) const {
    if (expires < core::Seconds{1} || expires > kMaxPresignExpiry) {
        return std::unexpected(PresignError::ExpiryOutOfRange);
    }
    const AmzDate date(now);
    RequestTarget presigned = target;
    auto& query = presigned.query;
    query.push_back({.name = "X-Amz-Algorithm", .value = std::string(kAlgorithm)});
    query.push_back({.name = "X-Amz-Credential",
                     .value = std::string(credentials.access_key_id()) + '/' +
                              credential_scope(date, region_)});
    query.push_back({.name = "X-Amz-Date", .value = std::string(date.datetime())});
    query.push_back({.name = "X-Amz-Expires", .value = std::to_string(expires.count())});
    if (const auto& token = credentials.session_token()) {
        query.push_back({.name = "X-Amz-Security-Token", .value = std::string(token->reveal())});
    }
    query.push_back({.name = "X-Amz-SignedHeaders", .value = "host"});

    const std::array headers{Header{.name = "host", .value = presigned.host}};
    const std::string canonical = canonical_request(method, presigned, headers, kUnsignedPayload);
    // Appended rather than sorted in: the signature cannot be part of what it signs.
    return to_url(presigned) + "&X-Amz-Signature=" + signature(date, canonical, credentials);
}

} // namespace infra::s3util
