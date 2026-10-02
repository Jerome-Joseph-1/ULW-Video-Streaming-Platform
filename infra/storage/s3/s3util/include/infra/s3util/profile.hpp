#pragma once

#include "core/util/time.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace infra::s3util {

enum class Addressing : std::uint8_t { PathStyle, VirtualHosted };

enum class ProfileError : std::uint8_t {
    InvalidEndpoint,
    InvalidAccountId,
    InvalidBucket,
};

struct Endpoint {
    // "https", or "http" for a local MinIO only. Plain HTTP is never chosen here: it is the
    // configured endpoint's own spelling, copied from it, so it is only ever an operator's.
    std::string scheme = "https";
    std::string host;
    std::uint16_t port = 443;
};

// Accepts exactly "http[s]://host[:port]" with a lowercase DNS name or IPv4 host; no path,
// userinfo or IPv6 literal.
[[nodiscard]] std::expected<Endpoint, ProfileError> parse_endpoint(std::string_view url);

// The Host header value. A default port is left out because clients omit it and SigV4 signs
// the header byte for byte.
[[nodiscard]] std::string authority(const Endpoint& endpoint);

struct S3Profile {
    Endpoint endpoint;
    std::string region;
    Addressing addressing = Addressing::PathStyle;
    // 5 MiB: S3's floor for every part but the last.
    std::uint64_t min_part_bytes = std::uint64_t{5} << 20U;
    // 5 GiB: S3's ceiling for a single part.
    std::uint64_t max_part_bytes = std::uint64_t{5} << 30U;
    std::uint32_t max_parts = 10'000;
    bool uniform_parts_required = false;
    // 7 days, the longest X-Amz-Expires SigV4 accepts.
    core::Seconds max_presign_ttl{604'800};
    bool supports_conditional_put = false;

    [[nodiscard]] static std::expected<S3Profile, ProfileError> minio(std::string_view endpoint);
    [[nodiscard]] static std::expected<S3Profile, ProfileError> r2(std::string_view account_id);
};

} // namespace infra::s3util
