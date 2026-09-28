#pragma once

#include "core/errors/domain_error.hpp"
#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <cstdint>
#include <expected>

namespace core {

enum class UploadState : std::uint8_t { Active, Completed, Aborted };

// An upload as the repository stores it. Upload::rehydrate decides whether it is consistent.
struct UploadRecord {
    UploadId id;
    VideoId video_id;
    UserId owner;
    std::uint64_t size_bytes = 0;
    std::uint64_t chunk_size = 0;
    std::uint64_t durable_offset = 0;
    UploadState state = UploadState::Active;
    WallTime expires_at;
};

// Named fields so a caller cannot swap the two sizes without it showing at the call site.
struct UploadParams {
    UploadId id;
    VideoId video_id;
    UserId owner;
    std::uint64_t size_bytes = 0;
    std::uint64_t chunk_size = 0;
    WallTime expires_at;
};

// One resumable upload. The durable offset never moves backwards, so a client resuming from any
// offset the server once reported cannot leave a gap. Rejected calls leave the object untouched.
class Upload {
public:
    // 50 GiB: a product cap on one source file, not a storage limit.
    static constexpr std::uint64_t kMaxSizeBytes = 50ULL << 30U;

    [[nodiscard]] static std::expected<Upload, DomainError> create(const UploadParams& params);
    [[nodiscard]] static std::expected<Upload, DomainError> rehydrate(const UploadRecord& record);

    // Moving to the current offset succeeds without effect: a retried progress report is not an
    // error.
    [[nodiscard]] std::expected<void, DomainError> advance_to(std::uint64_t new_offset) noexcept;
    [[nodiscard]] std::expected<void, DomainError> complete() noexcept;
    [[nodiscard]] std::expected<void, DomainError> abort() noexcept;

    [[nodiscard]] bool is_expired(WallTime now) const noexcept { return now >= data_.expires_at; }
    [[nodiscard]] bool owned_by(const UserId& user) const noexcept { return data_.owner == user; }

    [[nodiscard]] const UploadId& id() const noexcept { return data_.id; }
    [[nodiscard]] const VideoId& video_id() const noexcept { return data_.video_id; }
    [[nodiscard]] const UserId& owner() const noexcept { return data_.owner; }
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return data_.size_bytes; }
    [[nodiscard]] std::uint64_t chunk_size() const noexcept { return data_.chunk_size; }
    [[nodiscard]] std::uint64_t durable_offset() const noexcept { return data_.durable_offset; }
    [[nodiscard]] UploadState state() const noexcept { return data_.state; }
    [[nodiscard]] WallTime expires_at() const noexcept { return data_.expires_at; }

private:
    explicit Upload(const UploadRecord& data) noexcept : data_(data) {}

    UploadRecord data_;
};

} // namespace core
