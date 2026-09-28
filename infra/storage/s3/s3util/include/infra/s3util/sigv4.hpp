#pragma once

#include "core/util/time.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/crypto.hpp"
#include "infra/s3util/url.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace infra::s3util {

// Lets a streamed body go out without hashing it first. TLS already protects its integrity,
// so this is only for https endpoints.
inline constexpr std::string_view kUnsignedPayload = "UNSIGNED-PAYLOAD";

// sha256 of zero bytes, for requests without a body.
inline constexpr std::string_view kEmptyPayloadSha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

[[nodiscard]] std::string payload_sha256(std::span<const std::byte> body);

// x-amz-date, "YYYYMMDDTHHMMSSZ" in UTC; the first eight characters are the scope date.
class AmzDate {
public:
    explicit AmzDate(core::WallTime t) noexcept;

    [[nodiscard]] std::string_view datetime() const noexcept {
        return {text_.data(), text_.size()};
    }
    [[nodiscard]] std::string_view date() const noexcept { return datetime().substr(0, 8); }

private:
    std::array<char, 16> text_{};
};

struct Header {
    std::string name;
    std::string value;
};

// Headers are lowercased, trimmed, collapsed and sorted here, so callers may pass them in any
// form. Exposed so a SignatureDoesNotMatch can be diagnosed against S3's own version.
[[nodiscard]] std::string canonical_request(std::string_view method, const RequestTarget& target,
                                            std::span<const Header> headers,
                                            std::string_view payload_hash);

[[nodiscard]] Sha256Digest derive_signing_key(const SecretString& secret, const AmzDate& date,
                                              std::string_view region);

enum class PresignError : std::uint8_t { ExpiryOutOfRange };

// AWS Signature Version 4 for service "s3" in one region. Safe to share between threads.
class Signer {
public:
    explicit Signer(std::string region);
    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;
    Signer(Signer&&) noexcept;
    Signer& operator=(Signer&&) noexcept;
    ~Signer();

    // Returns every header the request must carry, which is exactly the set that was signed:
    // the caller's, host, x-amz-content-sha256, x-amz-date, x-amz-security-token when the
    // credentials have one, and authorization last. The caller must not pass any of those.
    [[nodiscard]] std::vector<Header>
    sign(std::string_view method, const RequestTarget& target, std::span<const Header> headers,
         std::string_view payload_hash, const Credentials& credentials, core::WallTime now) const;

    // A complete URL that grants `method` on the target to whoever holds it until it expires.
    [[nodiscard]] std::expected<std::string, PresignError>
    presign(std::string_view method, const RequestTarget& target, core::Seconds expires,
            const Credentials& credentials, core::WallTime now) const;

private:
    class KeyCache;

    [[nodiscard]] std::string signature(const AmzDate& date, std::string_view canonical,
                                        const Credentials& credentials) const;

    std::string region_;
    std::unique_ptr<KeyCache> cache_;
};

} // namespace infra::s3util
