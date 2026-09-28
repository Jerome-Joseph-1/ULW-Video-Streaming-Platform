#include "live_s3.hpp"

#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include <array>
#include <cstdlib>
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
