#include "live_s3.hpp"

#include "core/models/storage_key.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>

namespace ulw::test {

namespace {

std::string env_or(const char* name, std::string_view fallback) {
    // Read once per harness, before any thread that could setenv exists.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* const value = std::getenv(name);
    return value == nullptr || *value == '\0' ? std::string(fallback) : std::string(value);
}

infra::s3util::StaticCredentialProvider credentials(std::string_view access_key,
                                                    std::string_view secret) {
    return infra::s3util::StaticCredentialProvider(
        *infra::s3util::Credentials::make(access_key, infra::s3util::SecretString(secret)));
}

} // namespace

LiveS3 minio_from_env() {
    // Throwaway credentials, the same ones deploy/local starts MinIO with.
    return LiveS3{.name = "minio",
                  .profile = *infra::s3util::S3Profile::minio(
                      env_or("ULW_MINIO_ENDPOINT", "http://127.0.0.1:9000")),
                  .bucket = "ulw-test",
                  .credentials = credentials(env_or("ULW_MINIO_ACCESS_KEY", "ulw-dev"),
                                             env_or("ULW_MINIO_SECRET_KEY", "ulw-dev-secret"))};
}

std::optional<LiveS3> r2_from_env() {
    const std::string account = env_or("ULW_R2_ACCOUNT_ID", "");
    const std::string key_id = env_or("ULW_R2_ACCESS_KEY_ID", "");
    const std::string secret = env_or("ULW_R2_SECRET_ACCESS_KEY", "");
    const std::string bucket = env_or("ULW_R2_BUCKET", "");
    if (account.empty() || key_id.empty() || secret.empty() || bucket.empty()) {
        return std::nullopt;
    }
    auto profile = infra::s3util::S3Profile::r2(account);
    if (!profile) {
        return std::nullopt;
    }
    return LiveS3{.name = "r2",
                  .profile = *std::move(profile),
                  .bucket = bucket,
                  .credentials = credentials(key_id, secret)};
}

infra::curl::Result send(const LiveS3& target, infra::curl::Method method,
                         const infra::s3util::RequestTarget& request,
                         std::span<const std::byte> body,
                         std::span<const infra::s3util::Header> headers,
                         std::optional<std::string_view> payload_hash, std::size_t max_body) {
    static const os::SystemClock clock;
    const infra::s3util::Signer signer(target.profile.region);
    const std::string hash =
        payload_hash ? std::string(*payload_hash) : infra::s3util::payload_sha256(body);
    infra::curl::Request out{.method = method,
                             .url = infra::s3util::to_url(request),
                             .headers = {},
                             .max_body = max_body};
    for (const auto& h : signer.sign(infra::curl::to_string(method), request, headers, hash,
                                     target.credentials.credentials(), clock.wall_now())) {
        out.headers.push_back(h.name + ": " + h.value);
    }
    return infra::curl::perform(out, body);
}

bool ensure_bucket(const LiveS3& target) {
    const auto bucket = infra::s3util::Bucket::make(target.profile, target.bucket);
    if (!bucket) {
        return false;
    }
    const auto head = send(target, infra::curl::Method::Head, bucket->root());
    if (!head) {
        return false;
    }
    if (head->status == 200) {
        return true;
    }
    const auto created = send(target, infra::curl::Method::Put, bucket->root());
    // 409 is BucketAlreadyOwnedByYou from a run that raced this one.
    return created && (created->status == 200 || created->status == 409);
}

std::optional<std::vector<infra::s3util::MultipartUpload>> open_uploads(const LiveS3& target,
                                                                        std::string_view prefix) {
    const auto bucket = infra::s3util::Bucket::make(target.profile, target.bucket);
    if (!bucket) {
        return std::nullopt;
    }
    std::vector<infra::s3util::MultipartUpload> uploads;
    std::optional<std::string> key_marker;
    std::optional<std::string> upload_marker;
    while (true) {
        // Filtered here rather than with a prefix parameter: MinIO lists nothing for a prefix
        // that is not a whole object key.
        std::vector<infra::s3util::QueryParam> query{{.name = "uploads", .value = ""}};
        if (key_marker && upload_marker) {
            query.push_back({.name = "key-marker", .value = *key_marker});
            query.push_back({.name = "upload-id-marker", .value = *upload_marker});
        }
        const auto r = send(target, infra::curl::Method::Get, bucket->root(std::move(query)));
        if (!r || r->status != 200) {
            return std::nullopt;
        }
        auto page = infra::s3util::parse_list_multipart_uploads(r->body);
        if (!page) {
            return std::nullopt;
        }
        for (auto& upload : page->uploads) {
            if (upload.key.starts_with(prefix)) {
                uploads.push_back(std::move(upload));
            }
        }
        if (!page->is_truncated) {
            return uploads;
        }
        if (page->next_key_marker == key_marker && page->next_upload_id_marker == upload_marker) {
            return std::nullopt;
        }
        key_marker = std::move(page->next_key_marker);
        upload_marker = std::move(page->next_upload_id_marker);
    }
}

void abort_uploads(const LiveS3& target, std::string_view prefix) {
    const auto bucket = infra::s3util::Bucket::make(target.profile, target.bucket);
    const auto uploads = open_uploads(target, prefix);
    if (!bucket || !uploads) {
        return;
    }
    for (const auto& upload : *uploads) {
        const auto key = core::StorageKey::parse(upload.key);
        if (key) {
            [[maybe_unused]] const auto aborted =
                send(target, infra::curl::Method::Delete,
                     bucket->object(*key, {{.name = "uploadId", .value = upload.upload_id}}));
        }
    }
}

void remove_objects(const LiveS3& target, std::string_view prefix) {
    const auto bucket = infra::s3util::Bucket::make(target.profile, target.bucket);
    if (!bucket) {
        return;
    }
    const auto listed = send(target, infra::curl::Method::Get,
                             bucket->root({{.name = "list-type", .value = "2"},
                                           {.name = "prefix", .value = std::string(prefix)}}));
    if (!listed || listed->status != 200) {
        return;
    }
    const auto page = infra::s3util::parse_list_objects_v2(listed->body);
    if (!page) {
        return;
    }
    for (const std::string& name : page->keys) {
        if (const auto key = core::StorageKey::parse(name)) {
            [[maybe_unused]] const auto removed =
                send(target, infra::curl::Method::Delete, bucket->object(*key));
        }
    }
}

std::string unique_prefix(std::string_view what) {
    os::SystemRandom random;
    std::array<std::byte, 8> raw{};
    random.fill(raw);
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string prefix(what);
    prefix.push_back('-');
    for (const std::byte b : raw) {
        prefix.push_back(kHex[std::to_integer<std::size_t>(b) >> 4U]);
        prefix.push_back(kHex[std::to_integer<std::size_t>(b) & 0xFU]);
    }
    prefix.push_back('/');
    return prefix;
}

} // namespace ulw::test
