#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "core/ports/storage.hpp"
#include "core/util/time.hpp"
#include "infra/curl/multi.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/profile.hpp"
#include "infra/s3util/retry.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace infra::storage {

namespace s3 {
class Control;
class Endpoint;
class PageCount;
} // namespace s3

struct S3StoreOptions {
    // ADR-0009's 8 MiB chunk: a 50 GiB upload is 6,400 parts, inside S3's 10,000.
    std::uint64_t part_size = std::uint64_t{8} << 20U;
    // At most 0.1 + 0.2 + ... + 3.2 = 6.3 s of backoff before a control operation gives up:
    // long enough to ride out a throttling burst, short enough that the client asking is
    // still waiting for the answer.
    s3util::RetryPolicy::Config retry{
        .base = core::Millis{100}, .cap = core::Millis{5000}, .max_retries = 6};
};

enum class S3ConfigError : std::uint8_t { InvalidBucket, PartSizeOutOfRange, InvalidRetryPolicy };

// An ingest is an S3 multipart upload: backend_ref is the upload id, chunk_size the part size,
// and the durable offset counts the leading parts S3 lists as complete. Reads are presigned
// GET URLs. open(), its sessions and grant_read() run on the reactor; every other call blocks
// on the network, retrying transient failures with jittered backoff, and belongs on the
// offload pool.
class S3Store final : public core::ports::IIngestStore,
                      public core::ports::IObjectReader,
                      public core::ports::IObjectAdmin {
    struct Token {
        explicit Token() = default;
    };

public:
    struct Deps {
        net::IReactor& reactor;
        curl::Multi& multi;
        const s3util::ICredentialProvider& credentials;
        const core::ports::IClock& clock;
        // Draws retry jitter on pool threads, so it must be thread-safe.
        core::ports::IRandom& random;
        s3util::S3Profile profile;
        std::string bucket;
    };

    [[nodiscard]] static std::expected<std::unique_ptr<S3Store>, S3ConfigError>
    create(Deps deps, const S3StoreOptions& options = {});

    S3Store(Token token, Deps deps, std::uint64_t part_size, std::unique_ptr<s3::PageCount> pages,
            std::unique_ptr<s3::Endpoint> endpoint, std::unique_ptr<s3::Control> control);
    // Sessions must be destroyed first.
    ~S3Store() override;
    S3Store(const S3Store&) = delete;
    S3Store& operator=(const S3Store&) = delete;

    [[nodiscard]] std::expected<core::ports::IngestId, core::ports::StorageError>
    create(const core::StorageKey& key, std::uint64_t total_bytes,
           const core::ContentType& type) override;
    [[nodiscard]] std::expected<std::unique_ptr<core::ports::IIngestSession>,
                                core::ports::StorageError>
    open(const core::ports::IngestId& id, std::uint64_t offset,
         core::ports::IIngestObserver& observer) override;
    [[nodiscard]] std::expected<std::uint64_t, core::ports::StorageError>
    durable_offset(const core::ports::IngestId& id) override;
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    commit(const core::ports::IngestId& id) override;
    void discard(const core::ports::IngestId& id) noexcept override;
    [[nodiscard]] std::uint64_t preferred_chunk_size() const noexcept override {
        return part_size_;
    }
    // Failures only a fix on our side can cure: S3 refused our signature, our credentials or
    // clock, or the bucket we are configured with. Whoever hit one saw an ordinary error, so a
    // metric on this count is what should page someone. Safe from any thread.
    [[nodiscard]] std::uint64_t paging_errors() const noexcept;

    // The ttl is capped at the profile's longest presign; the grant reports the one used.
    [[nodiscard]] std::expected<core::ports::ReadGrant, core::ports::StorageError>
    grant_read(const core::StorageKey& key, core::Seconds ttl) override;
    [[nodiscard]] std::expected<std::vector<std::byte>, core::ports::StorageError>
    fetch_small(const core::StorageKey& key, std::size_t max) override;

    [[nodiscard]] std::expected<void, core::ports::StorageError>
    put(const core::StorageKey& key, std::span<const std::byte> bytes) override;
    [[nodiscard]] std::expected<void, core::ports::StorageError>
    remove(const core::StorageKey& key) override;
    [[nodiscard]] std::expected<std::vector<core::StorageKey>, core::ports::StorageError>
    list(std::string_view prefix) override;
    // Aborts every multipart upload in the bucket started before `older_than` and returns how
    // many it aborted. One whose abort fails is left for the next pass.
    [[nodiscard]] std::expected<std::size_t, core::ports::StorageError>
    reap_abandoned(core::WallTime older_than) override;

private:
    Deps deps_;
    std::uint64_t part_size_;
    // Declared before the control, which notes failures in it, so that it outlives it.
    std::unique_ptr<s3::PageCount> pages_;
    std::unique_ptr<s3::Endpoint> endpoint_;
    std::unique_ptr<s3::Control> control_;
};

} // namespace infra::storage
