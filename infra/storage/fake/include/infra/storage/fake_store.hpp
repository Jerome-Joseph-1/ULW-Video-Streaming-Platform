#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/storage.hpp"
#include "net/reactor.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace infra::storage {

// Faults the in-memory store injects, for exercising the paths real backends only take
// under load or failure.
struct FaultPlan {
    // The first N chunk uploads come back throttled; the adapter retries them internally.
    std::size_t throttle_first = 0;
    // Chunk number (1-based, counted per ingest) that fails permanently; 0 for none.
    std::uint64_t fail_chunk = 0;
    core::ports::StorageError fail_error = core::ports::StorageError::Permanent;
    // Most bytes a single write() takes.
    std::size_t accept_per_call = std::numeric_limits<std::size_t>::max();
    // A backend that has stalled outright: write() takes nothing, ever.
    bool accept_zero = false;
    // Every fetch_small fails with this.
    std::optional<core::ports::StorageError> fail_fetch = std::nullopt;
};

// In-memory ingest store with S3-family semantics: bytes become durable a whole chunk at a
// time (or at the end of the object), each chunk "uploads" asynchronously on the reactor,
// and commit assembles the chunks. Control operations may run on other threads.
class FakeStore final : public core::ports::IIngestStore,
                        public core::ports::IObjectReader,
                        public core::ports::IObjectAdmin {
public:
    FakeStore(net::IReactor& reactor, const core::ports::IClock& clock, std::uint64_t chunk_size,
              FaultPlan plan = {});
    ~FakeStore() override;
    FakeStore(const FakeStore&) = delete;
    FakeStore& operator=(const FakeStore&) = delete;

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
        return chunk_size_;
    }

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
    [[nodiscard]] std::expected<std::size_t, core::ports::StorageError>
    reap_abandoned(core::WallTime older_than) override;

    void set_plan(const FaultPlan& plan);
    // Chunk upload attempts so far, retries included.
    [[nodiscard]] std::size_t chunk_attempts() const;

private:
    class Session;

    struct Ingest {
        core::StorageKey key;
        std::uint64_t total = 0;
        core::WallTime created;
        std::map<std::uint64_t, std::vector<std::byte>> chunks;
    };

    enum class Attempt { Stored, Throttled, Failed };

    [[nodiscard]] std::optional<core::ports::StorageError> check_resume(const std::string& ref,
                                                                        std::uint64_t offset) const;
    [[nodiscard]] Attempt store_chunk(const std::string& ref, std::uint64_t index,
                                      const std::vector<std::byte>& bytes,
                                      core::ports::StorageError& error);
    [[nodiscard]] static std::uint64_t contiguous_bytes(const Ingest& ingest) noexcept;
    [[nodiscard]] std::uint64_t chunk_length(std::uint64_t total,
                                             std::uint64_t index) const noexcept;

    net::IReactor& reactor_;
    const core::ports::IClock& clock_;
    const std::uint64_t chunk_size_;

    mutable std::mutex mutex_;
    FaultPlan plan_;
    std::size_t attempts_ = 0;
    std::size_t throttled_ = 0;
    std::uint64_t next_ref_ = 1;
    std::map<std::string, Ingest> ingests_;
    std::set<std::string> committed_;
    std::map<std::string, std::vector<std::byte>> objects_;
};

} // namespace infra::storage
