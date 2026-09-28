#include "endpoint.hpp"

#include <utility>
#include <vector>

namespace infra::storage::s3 {

Endpoint::Endpoint(s3util::Bucket bucket, const s3util::S3Profile& profile,
                   const s3util::ICredentialProvider& credentials, const core::ports::IClock& clock)
    : bucket_(std::move(bucket)), signer_(profile.region), credentials_(credentials),
      clock_(clock) {}

curl::Request Endpoint::sign(curl::Method method, const s3util::RequestTarget& target,
                             std::span<const s3util::Header> headers, std::string_view payload_hash,
                             std::size_t max_body) const {
    const std::vector<s3util::Header> all =
        signer_.sign(curl::to_string(method), target, headers, payload_hash,
                     credentials_.credentials(), clock_.wall_now());
    curl::Request request{
        .method = method, .url = s3util::to_url(target), .headers = {}, .max_body = max_body};
    request.headers.reserve(all.size());
    for (const s3util::Header& h : all) {
        request.headers.push_back(h.name + ": " + h.value);
    }
    return request;
}

std::expected<std::string, s3util::PresignError>
Endpoint::presign_get(const s3util::RequestTarget& target, core::Seconds ttl) const {
    return signer_.presign("GET", target, ttl, credentials_.credentials(), clock_.wall_now());
}

} // namespace infra::storage::s3
