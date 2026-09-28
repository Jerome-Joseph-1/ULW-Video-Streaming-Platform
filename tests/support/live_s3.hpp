#pragma once

#include "infra/curl/http.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/profile.hpp"
#include "infra/s3util/sigv4.hpp"
#include "infra/s3util/url.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ulw::test {

// A real S3-compatible bucket for the live suites.
struct LiveS3 {
    std::string name;
    infra::s3util::S3Profile profile;
    std::string bucket;
    infra::s3util::StaticCredentialProvider credentials;
};

// ULW_MINIO_ENDPOINT, ULW_MINIO_ACCESS_KEY and ULW_MINIO_SECRET_KEY, each defaulting to the
// MinIO of deploy/local/docker-compose.minio.yml; the bucket is ulw-test.
[[nodiscard]] LiveS3 minio_from_env();
// ULW_R2_ACCOUNT_ID, ULW_R2_ACCESS_KEY_ID, ULW_R2_SECRET_ACCESS_KEY and ULW_R2_BUCKET, or
// nothing unless all four are set.
[[nodiscard]] std::optional<LiveS3> r2_from_env();

// Signs with the target's credentials and sends. `payload_hash` defaults to the body's own.
[[nodiscard]] infra::curl::Result send(const LiveS3& target, infra::curl::Method method,
                                       const infra::s3util::RequestTarget& request,
                                       std::span<const std::byte> body = {},
                                       std::span<const infra::s3util::Header> headers = {},
                                       std::optional<std::string_view> payload_hash = {},
                                       std::size_t max_body = std::size_t{1} << 20U);

// Creates the bucket when it is missing. False when the endpoint is unreachable or refuses.
[[nodiscard]] bool ensure_bucket(const LiveS3& target);

// "<what>-<16 hex digits>/", fresh per call: no two runs, concurrent or not, share a key, which
// also keeps R2's one-write-per-second-per-key limit out of reach.
[[nodiscard]] std::string unique_prefix(std::string_view what);

} // namespace ulw::test
