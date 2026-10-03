#pragma once

#include "core/util/time.hpp"
#include "infra/webpush/ece.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

// Voluntary Application Server Identification (RFC 8292): every push request carries a JWT the
// application server signs with its own P-256 key, for the push service's origin, and the key's
// public half. A browser subscribed with that public key (applicationServerKey) accepts pushes
// only from whoever holds the private half; the push service checks the signature. The key is
// the operator's, from a Secret, and nothing here logs it or anything derived from it but the
// public key.
namespace infra::webpush {

// RFC 8292 section 2: no more than 24 hours ahead. Half of that leaves any clock skew room.
inline constexpr core::Seconds kVapidLifetime{12 * 3600};
// A contact URI the push service may use: "mailto:" or "https://", printable ASCII, at most this
// long.
inline constexpr std::size_t kMaxVapidSubject = 256;

enum class VapidError : std::uint8_t {
    // Not base64url, not 32 bytes, or not a scalar of P-256 (zero, or the group order or more).
    BadKey,
    // OpenSSL could not sign.
    Crypto,
};

// Whether `subject` is a contact URI a VAPID claim may carry (RFC 8292 section 2.1).
[[nodiscard]] bool is_vapid_subject(std::string_view subject) noexcept;

class VapidKey {
public:
    // The private scalar in unpadded base64url, 43 characters: what `web-push generate-vapid-keys`
    // and most Web Push libraries print as the private key.
    [[nodiscard]] static std::expected<VapidKey, VapidError> from_base64url(std::string_view text);
    [[nodiscard]] static std::expected<VapidKey, VapidError> from_private(const PrivateKey& key);

    VapidKey(VapidKey&&) noexcept;
    VapidKey& operator=(VapidKey&&) noexcept;
    VapidKey(const VapidKey&) = delete;
    VapidKey& operator=(const VapidKey&) = delete;
    ~VapidKey();

    // The applicationServerKey clients subscribe with, unpadded base64url of the uncompressed
    // point.
    [[nodiscard]] const std::string& public_key() const noexcept { return public_text_; }

    // The JWT for the push service at `audience` ("https://host", the endpoint's origin), with
    // `subject` as its sub claim, expiring at `expires` (whole seconds).
    [[nodiscard]] std::expected<std::string, VapidError>
    token(std::string_view audience, std::string_view subject, core::WallTime expires) const;
    // The Authorization header's value: "vapid t=<token>, k=<public key>".
    [[nodiscard]] std::expected<std::string, VapidError>
    authorization(std::string_view audience, std::string_view subject,
                  core::WallTime expires) const;

private:
    struct Key;
    explicit VapidKey(std::unique_ptr<Key> key, std::string public_text) noexcept;

    std::unique_ptr<Key> key_;
    std::string public_text_;
};

} // namespace infra::webpush
