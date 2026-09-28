#include "core/models/upload.hpp"

#include "core/errors/domain_error.hpp"

#include <cstdint>
#include <expected>

namespace core {

namespace {

[[nodiscard]] std::expected<void, DomainError> validate(const UploadRecord& record) noexcept {
    if (record.size_bytes == 0 || record.size_bytes > Upload::kMaxSizeBytes) {
        return std::unexpected(DomainError::InvalidUploadSize);
    }
    if (record.chunk_size == 0) {
        return std::unexpected(DomainError::InvalidChunkSize);
    }
    if (record.durable_offset > record.size_bytes) {
        return std::unexpected(DomainError::OffsetBeyondSize);
    }
    if (record.state == UploadState::Completed && record.durable_offset != record.size_bytes) {
        return std::unexpected(DomainError::CorruptRecord);
    }
    return {};
}

} // namespace

std::expected<Upload, DomainError> Upload::create(const UploadParams& params) {
    return rehydrate(UploadRecord{.id = params.id,
                                  .video_id = params.video_id,
                                  .owner = params.owner,
                                  .size_bytes = params.size_bytes,
                                  .chunk_size = params.chunk_size,
                                  .durable_offset = 0,
                                  .state = UploadState::Active,
                                  .expires_at = params.expires_at});
}

std::expected<Upload, DomainError> Upload::rehydrate(const UploadRecord& record) {
    if (const auto ok = validate(record); !ok) {
        return std::unexpected(ok.error());
    }
    return Upload{record};
}

std::expected<void, DomainError> Upload::advance_to(std::uint64_t new_offset) noexcept {
    if (data_.state != UploadState::Active) {
        return std::unexpected(DomainError::UploadNotActive);
    }
    if (new_offset < data_.durable_offset) {
        return std::unexpected(DomainError::OffsetRegression);
    }
    if (new_offset > data_.size_bytes) {
        return std::unexpected(DomainError::OffsetBeyondSize);
    }
    data_.durable_offset = new_offset;
    return {};
}

std::expected<void, DomainError> Upload::complete() noexcept {
    if (data_.state != UploadState::Active) {
        return std::unexpected(DomainError::UploadNotActive);
    }
    if (data_.durable_offset != data_.size_bytes) {
        return std::unexpected(DomainError::UploadIncomplete);
    }
    data_.state = UploadState::Completed;
    return {};
}

std::expected<void, DomainError> Upload::abort() noexcept {
    if (data_.state != UploadState::Active) {
        return std::unexpected(DomainError::UploadNotActive);
    }
    data_.state = UploadState::Aborted;
    return {};
}

} // namespace core
