#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/object_stream.hpp"
#include "core/ports/object_transfer.hpp"
#include "core/ports/random.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/profile.hpp"
#include "infra/s3util/retry.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace infra::storage {

namespace s3 {
class Control;
class Endpoint;
class PageCount;
} // namespace s3

enum class S3TransferConfigError : std::uint8_t { InvalidBucket, InvalidRetryPolicy };

// IObjectTransfer on an S3-family bucket with blocking libcurl calls, for processes without a
// reactor. Downloads stream to disk; uploads are one PUT streamed from disk and signed over the
// file's SHA-256, which is hashed first in a separate pass, so plain-HTTP endpoints (MinIO in
// development) verify the body too. Transient failures are retried with jittered backoff.
// Streams are multipart uploads of stream_part_bytes() parts, each held in memory until it is
// sent; a stream must not outlive the transfer that began it.
class S3Transfer final : public core::ports::IObjectTransfer, public core::ports::IObjectStreams {
    struct Token {
        explicit Token() = default;
    };

public:
    struct Deps {
        const s3util::ICredentialProvider& credentials;
        const core::ports::IClock& clock;
        core::ports::IRandom& random;
        s3util::S3Profile profile;
        std::string bucket;
    };

    // At most 0.1 + 0.2 + ... + 3.2 = 6.3 s of backoff per call, as for the store's control
    // operations: a job that cannot reach its bucket for longer fails and is retried whole.
    static constexpr s3util::RetryPolicy::Config kDefaultRetry{
        .base = core::Millis{100}, .cap = core::Millis{5000}, .max_retries = 6};

    // A stream's parts: at least 16 MiB, and enough larger that `max_bytes` fits in 9,000 of
    // S3's 10,000, in whole MiB. The thousand to spare absorb a stream a little past its bound
    // on the way to being refused. One part is held in memory at a time.
    [[nodiscard]] static std::uint64_t stream_part_bytes(std::uint64_t max_bytes) noexcept;

    [[nodiscard]] static std::expected<std::unique_ptr<S3Transfer>, S3TransferConfigError>
    create(Deps deps, const s3util::RetryPolicy::Config& retry = kDefaultRetry);

    S3Transfer(Token token, Deps deps, std::unique_ptr<s3::PageCount> pages,
               std::unique_ptr<s3::Endpoint> endpoint, std::unique_ptr<s3::Control> control);
    ~S3Transfer() override;
    S3Transfer(const S3Transfer&) = delete;
    S3Transfer& operator=(const S3Transfer&) = delete;
    S3Transfer(S3Transfer&&) = delete;
    S3Transfer& operator=(S3Transfer&&) = delete;

    [[nodiscard]] std::expected<std::uint64_t, core::ports::StorageError>
    size(const core::StorageKey& key) override;
    [[nodiscard]] std::expected<std::uint64_t, core::ports::StorageError>
    download(const core::StorageKey& key, const std::filesystem::path& destination) override;
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    upload(const std::filesystem::path& source, const core::StorageKey& key,
           const core::ContentType& type) override;
    // One PUT with If-None-Match: *, which S3, R2 and MinIO honour (Permanent on a profile that
    // does not).
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    upload_new(const std::filesystem::path& source, const core::StorageKey& key,
               const core::ContentType& type) override;
    [[nodiscard]] std::expected<std::unique_ptr<core::ports::IObjectStream>,
                                core::ports::StorageError>
    begin(const core::StorageKey& key, const core::ContentType& type,
          std::uint64_t max_bytes) override;
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    remove(const core::StorageKey& key) override;

    // Failures only a person can fix (a bad signature, credentials, a missing bucket).
    [[nodiscard]] std::uint64_t paging_errors() const noexcept;

private:
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    put(const std::filesystem::path& source, const core::StorageKey& key,
        const core::ContentType& type, bool create_only);

    Deps deps_;
    // Before control_, which counts into it.
    std::unique_ptr<s3::PageCount> pages_;
    std::unique_ptr<s3::Endpoint> endpoint_;
    std::unique_ptr<s3::Control> control_;
};

} // namespace infra::storage
