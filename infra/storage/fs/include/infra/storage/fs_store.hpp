#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/random.hpp"
#include "core/ports/storage.hpp"
#include "net/offload_pool.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace infra::storage {

// Local-filesystem backend for development and tests. Layout under the root:
//   objects/<key>              committed objects
//   ingest/<ref>/data          bytes received so far
//   ingest/<ref>/durable       offset known to be on disk (updated after fdatasync)
//   ingest/<ref>/meta          key, total size and creation time
//   ingest/<ref>/committed     present once commit has decided to move data into objects/
//   staging/                   files being written, renamed into place once complete
// File I/O never runs on the reactor thread: writes go to the store's own offload pool, and
// the control operations block by contract and are called from the caller's pool.
//
// The store owns the pool its writes run on, so that it can stop those threads before it
// frees the jobs they hold, whatever order the caller tears things down in. The reactor that
// pool completes on must outlive the store, and every session must be destroyed before it.
class FsStore final : public core::ports::IIngestStore,
                      public core::ports::IObjectReader,
                      public core::ports::IObjectAdmin {
public:
    struct Deps {
        const core::ports::IClock& clock;
        core::ports::IRandom& random;
    };

    // Throws std::invalid_argument for a missing writer or a zero chunk size.
    FsStore(Deps deps, std::unique_ptr<net::OffloadPool> writer, std::filesystem::path root,
            std::uint64_t chunk_size);
    ~FsStore() override;
    FsStore(const FsStore&) = delete;
    FsStore& operator=(const FsStore&) = delete;

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

private:
    class Session;
    class WriteJob;

    [[nodiscard]] std::filesystem::path ingest_dir(const std::string& ref) const;
    [[nodiscard]] std::filesystem::path object_path(const core::StorageKey& key) const;
    // Thread-safe.
    [[nodiscard]] std::filesystem::path staging_name();
    [[nodiscard]] static bool valid_ref(const std::string& ref);
    WriteJob& adopt_job(std::unique_ptr<WriteJob> job);
    void finish_job(WriteJob& job) noexcept;

    Deps deps_;
    std::filesystem::path root_;
    std::uint64_t chunk_size_;
    const std::string staging_prefix_;
    std::atomic<std::uint64_t> staged_{0};
    // Commits are a few metadata operations each; one lock for every upload is simpler than
    // one per upload, and concurrent commits of one upload must not both try the move.
    std::mutex commit_mutex_;
    // Reactor-thread only. Jobs outlive the sessions that started them when a session is
    // aborted mid-write, so the store owns them.
    std::vector<std::unique_ptr<WriteJob>> jobs_;
    // Last member, so it goes first: its threads finish the job in hand and drop the rest
    // before any job, or anything a job touches, is freed.
    std::unique_ptr<net::OffloadPool> writer_;
};

} // namespace infra::storage
