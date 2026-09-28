#include "infra/s3util/url.hpp"

#include "core/models/storage_key.hpp"
#include "infra/s3util/profile.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace infra::s3util {

namespace {

bool is_unreserved(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

std::string encode(std::string_view text, bool keep_slash) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        if (is_unreserved(ch) || (keep_slash && ch == '/')) {
            out.push_back(ch);
            continue;
        }
        const std::size_t byte = static_cast<unsigned char>(ch);
        out.push_back('%');
        out.push_back(kHex[byte >> 4U]);
        out.push_back(kHex[byte & 0xFU]);
    }
    return out;
}

bool is_lower_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// S3's bucket naming rules, minus the reserved prefixes and suffixes that only matter to AWS;
// what matters here is that the name cannot escape the host or the path. A dotted name is
// refused for virtual-hosted addressing because the certificate wildcard covers one label.
bool is_bucket_name(std::string_view name, Addressing addressing) noexcept {
    // 3..63: S3's length bounds, which also keep the name a single DNS label.
    if (name.size() < 3 || name.size() > 63 || !is_lower_alnum(name.front()) ||
        !is_lower_alnum(name.back()) || name.find("..") != std::string_view::npos) {
        return false;
    }
    const bool dots_allowed = addressing == Addressing::PathStyle;
    return std::ranges::all_of(name, [dots_allowed](char c) {
        return is_lower_alnum(c) || c == '-' || (dots_allowed && c == '.');
    });
}

} // namespace

std::string uri_encode(std::string_view text) {
    return encode(text, false);
}

std::string uri_encode_path(std::string_view path) {
    return encode(path, true);
}

std::string canonical_query(std::span<const QueryParam> query) {
    std::vector<std::pair<std::string, std::string>> encoded;
    encoded.reserve(query.size());
    for (const auto& param : query) {
        encoded.emplace_back(uri_encode(param.name), uri_encode(param.value));
    }
    std::ranges::sort(encoded);
    std::string out;
    for (const auto& [name, value] : encoded) {
        if (!out.empty()) {
            out.push_back('&');
        }
        out.append(name).append(1, '=').append(value);
    }
    return out;
}

std::string to_url(const RequestTarget& target) {
    std::string url = target.scheme == Scheme::Https ? "https://" : "http://";
    url.append(target.host).append(uri_encode_path(target.path));
    if (!target.query.empty()) {
        url.append(1, '?').append(canonical_query(target.query));
    }
    return url;
}

std::expected<Bucket, ProfileError> Bucket::make(const S3Profile& profile, std::string_view name) {
    if (!is_bucket_name(name, profile.addressing)) {
        return std::unexpected(ProfileError::InvalidBucket);
    }
    if (profile.addressing == Addressing::VirtualHosted) {
        return Bucket(profile.endpoint.scheme,
                      std::string(name) + '.' + authority(profile.endpoint), "");
    }
    return Bucket(profile.endpoint.scheme, authority(profile.endpoint), '/' + std::string(name));
}

RequestTarget Bucket::root(std::vector<QueryParam> query) const {
    return RequestTarget{.scheme = scheme_,
                         .host = host_,
                         .path = path_prefix_.empty() ? "/" : path_prefix_,
                         .query = std::move(query)};
}

RequestTarget Bucket::object(const core::StorageKey& key, std::vector<QueryParam> query) const {
    return RequestTarget{.scheme = scheme_,
                         .host = host_,
                         .path = path_prefix_ + '/' + key.str(),
                         .query = std::move(query)};
}

} // namespace infra::s3util
