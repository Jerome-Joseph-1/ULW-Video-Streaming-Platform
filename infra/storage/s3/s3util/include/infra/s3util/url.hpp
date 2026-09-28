#pragma once

#include "core/models/storage_key.hpp"
#include "infra/s3util/profile.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::s3util {

// Raw, unencoded. An empty value is a flag parameter such as "uploads".
struct QueryParam {
    std::string name;
    std::string value;
};

// Everything needed to put a request on the wire and to sign it. `path` is raw; it is encoded
// once, by the same function, for both the request line and the canonical request.
struct RequestTarget {
    Scheme scheme = Scheme::Https;
    std::string host;
    std::string path;
    std::vector<QueryParam> query;
};

// RFC 3986 unreserved characters pass through; every other byte becomes uppercase %XX.
[[nodiscard]] std::string uri_encode(std::string_view text);
[[nodiscard]] std::string uri_encode_path(std::string_view path);

// Names and values encoded, pairs sorted by encoded name then value, and every parameter
// written as name=value, including flags. S3 accepts this form on the wire as well.
[[nodiscard]] std::string canonical_query(std::span<const QueryParam> query);

[[nodiscard]] std::string to_url(const RequestTarget& target);

// A validated bucket on one backend. Resolving the addressing style here means no caller
// ever builds a host or path by hand.
class Bucket {
public:
    [[nodiscard]] static std::expected<Bucket, ProfileError> make(const S3Profile& profile,
                                                                  std::string_view name);

    [[nodiscard]] RequestTarget root(std::vector<QueryParam> query = {}) const;
    [[nodiscard]] RequestTarget object(const core::StorageKey& key,
                                       std::vector<QueryParam> query = {}) const;

private:
    Bucket(Scheme scheme, std::string host, std::string path_prefix)
        : scheme_(scheme), host_(std::move(host)), path_prefix_(std::move(path_prefix)) {}

    Scheme scheme_;
    std::string host_;
    // "/<bucket>" for path-style, empty for virtual-hosted.
    std::string path_prefix_;
};

} // namespace infra::s3util
