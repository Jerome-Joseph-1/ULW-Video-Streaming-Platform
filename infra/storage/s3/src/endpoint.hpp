#pragma once

#include "core/ports/clock.hpp"
#include "core/util/time.hpp"
#include "infra/curl/http.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/profile.hpp"
#include "infra/s3util/sigv4.hpp"
#include "infra/s3util/url.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace infra::storage::s3 {

// One bucket, and requests to it signed with whatever credentials are current. Safe to use
// from the reactor and the pool at once.
class Endpoint {
public:
    Endpoint(s3util::Bucket bucket, const s3util::S3Profile& profile,
             const s3util::ICredentialProvider& credentials, const core::ports::IClock& clock);

    [[nodiscard]] const s3util::Bucket& bucket() const noexcept { return bucket_; }

    // `payload_hash` is the hex SHA-256 of the body, or UNSIGNED-PAYLOAD for a streamed one.
    [[nodiscard]] curl::Request sign(curl::Method method, const s3util::RequestTarget& target,
                                     std::span<const s3util::Header> headers,
                                     std::string_view payload_hash, std::size_t max_body) const;
    [[nodiscard]] std::expected<std::string, s3util::PresignError>
    presign_get(const s3util::RequestTarget& target, core::Seconds ttl) const;

private:
    s3util::Bucket bucket_;
    s3util::Signer signer_;
    const s3util::ICredentialProvider& credentials_;
    const core::ports::IClock& clock_;
};

} // namespace infra::storage::s3
