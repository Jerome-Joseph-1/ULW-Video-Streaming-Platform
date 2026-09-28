#pragma once

#include "core/ports/storage.hpp"
#include "infra/curl/multi.hpp"
#include "net/reactor.hpp"

#include "endpoint.hpp"
#include "failure.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace infra::storage::s3 {

struct SessionDeps {
    net::IReactor& reactor;
    curl::Multi& multi;
    const Endpoint& endpoint;
    PageCount& pages;
};

// Streams an ingest into its multipart upload, one UploadPart in flight at a time. Bytes pass
// through a small ring on their way into the request body; a part becomes durable when S3
// answers it with 200 and an ETag, and the next part starts then.
//
// A part that fails is not retried here: its bytes are gone as soon as they are sent, so the
// session fails with the mapped error and the client re-sends from the durable offset.
class PartSession final : public core::ports::IIngestSession,
                          public curl::IBodySource,
                          public curl::ITransferHandler,
                          public net::ITimerHandler {
public:
    // The 64 KiB pump buffer ADR-0007 budgets per upload connection, which is also one full
    // libcurl upload read.
    static constexpr std::size_t kBufferBytes = std::size_t{64} << 10U;

    PartSession(const SessionDeps& deps, core::ports::IngestId id, std::uint64_t offset,
                core::ports::IIngestObserver& observer);
    ~PartSession() override;
    PartSession(const PartSession&) = delete;
    PartSession& operator=(const PartSession&) = delete;

    [[nodiscard]] std::size_t write(std::span<const std::byte> bytes) noexcept override;
    [[nodiscard]] bool wants_more() const noexcept override;
    void finish() noexcept override;
    [[nodiscard]] core::ports::IngestState state() const noexcept override { return state_; }
    [[nodiscard]] std::uint64_t durable_offset() const noexcept override { return durable_; }
    [[nodiscard]] std::optional<core::ports::StorageError> error() const noexcept override {
        return error_;
    }
    void abort() noexcept override;

    std::size_t read_body(std::span<std::byte> out) noexcept override;
    void on_transfer_done(curl::Result result) noexcept override;
    void on_timeout() noexcept override;

private:
    [[nodiscard]] std::uint64_t part_length() const noexcept;
    [[nodiscard]] std::uint64_t part_end() const noexcept { return part_start_ + part_length(); }
    // Starts the next part if its bytes are here, or settles a finishing session.
    void advance() noexcept;
    [[nodiscard]] bool start_part() noexcept;
    void fail(core::ports::StorageError error) noexcept;
    void drop_buffered() noexcept;
    void notify_later() noexcept;

    SessionDeps deps_;
    core::ports::IngestId id_;
    core::ports::IIngestObserver& observer_;

    std::vector<std::byte> ring_;
    std::size_t head_ = 0;
    std::size_t buffered_ = 0;
    // Just past the last byte write() took.
    std::uint64_t next_;
    // First byte of the part in flight, or of the next one to start.
    std::uint64_t part_start_;
    // Bytes of that part already handed to libcurl; the ring holds the ones after them.
    std::uint64_t sent_ = 0;
    std::uint64_t durable_;
    std::unique_ptr<curl::Transfer> transfer_;

    core::ports::IngestState state_ = core::ports::IngestState::Open;
    std::optional<core::ports::StorageError> error_;
    net::TimerId timer_{};
    // write() turned bytes away for want of room, so the writer is waiting to hear of some.
    bool blocked_ = false;
    bool aborted_ = false;
};

} // namespace infra::storage::s3
