#pragma once

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::ports {

enum class StorageError {
    NotFound,
    AlreadyExists,
    PreconditionFailed,
    Unauthorized,
    Throttled,
    Transient,
    Permanent,
    Corrupt,
};

[[nodiscard]] std::string_view to_string(StorageError e) noexcept;

enum class IngestState { Open, Finalizing, Committed, Failed };

// One resumable ingest. Only the key means anything outside the adapter; the rest is
// opaque adapter state that the caller persists verbatim and hands back unchanged.
struct IngestId {
    StorageKey key;
    std::string backend_ref;
    std::uint64_t total_bytes = 0;
    std::uint64_t chunk_size = 0;
};

class IIngestObserver {
public:
    // Runs on the reactor thread, never re-entrantly from inside a session call: write() may
    // now accept more bytes, or state() has changed.
    virtual void on_ingest_progress() noexcept = 0;

protected:
    ~IIngestObserver() = default;
};

// One in-flight chunk. Every member runs on the reactor thread and never blocks.
class IIngestSession {
public:
    virtual ~IIngestSession() = default;
    // Returns how many bytes were taken; 0 is backpressure, not failure.
    [[nodiscard]] virtual std::size_t write(std::span<const std::byte> bytes) noexcept = 0;
    [[nodiscard]] virtual bool wants_more() const noexcept = 0;
    virtual void finish() noexcept = 0;
    [[nodiscard]] virtual IngestState state() const noexcept = 0;
    // Monotonic, and always a legal offset to reopen at.
    [[nodiscard]] virtual std::uint64_t durable_offset() const noexcept = 0;
    [[nodiscard]] virtual std::optional<StorageError> error() const noexcept = 0;
    // Idempotent, safe from destructors; no observer calls follow it.
    virtual void abort() noexcept = 0;
};

// open() and the sessions it returns belong to the reactor thread. Every other member may
// block on the network and must be called from the offload pool; those are thread-safe.
class IIngestStore {
public:
    virtual ~IIngestStore() = default;
    [[nodiscard]] virtual std::expected<IngestId, StorageError>
    create(const StorageKey& key, std::uint64_t total_bytes, const ContentType& type) = 0;
    // `offset` must be a value durable_offset() has reported; the caller's cached copy is
    // fine because a stale-low offset only re-sends bytes the backend already holds.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IIngestSession>, StorageError>
    open(const IngestId& id, std::uint64_t offset, IIngestObserver& observer) = 0;
    [[nodiscard]] virtual std::expected<std::uint64_t, StorageError>
    durable_offset(const IngestId& id) = 0;
    // Idempotent. Fails with PreconditionFailed until every byte is durable.
    [[nodiscard]] virtual std::expected<void, StorageError> commit(const IngestId& id) = 0;
    virtual void discard(const IngestId& id) noexcept = 0;
    [[nodiscard]] virtual std::uint64_t preferred_chunk_size() const noexcept = 0;
};

struct ReadGrant {
    enum class Kind { RedirectUrl, ServeLocally };
    Kind kind;
    std::string value;
    Seconds ttl;
};

class IObjectReader {
public:
    virtual ~IObjectReader() = default;
    // Pure computation for URL-granting backends; safe on the reactor thread.
    [[nodiscard]] virtual std::expected<ReadGrant, StorageError> grant_read(const StorageKey& key,
                                                                            Seconds ttl) = 0;
    // Blocks; offload pool only. Objects larger than `max` fail with Permanent.
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, StorageError>
    fetch_small(const StorageKey& key, std::size_t max) = 0;
};

// Blocking administrative operations for the worker and the reaper.
class IObjectAdmin {
public:
    virtual ~IObjectAdmin() = default;
    [[nodiscard]] virtual std::expected<void, StorageError>
    put(const StorageKey& key, std::span<const std::byte> bytes) = 0;
    [[nodiscard]] virtual std::expected<void, StorageError> remove(const StorageKey& key) = 0;
    [[nodiscard]] virtual std::expected<std::vector<StorageKey>, StorageError>
    list(std::string_view prefix) = 0;
    [[nodiscard]] virtual std::expected<std::size_t, StorageError>
    reap_abandoned(WallTime older_than) = 0;
};

} // namespace core::ports
