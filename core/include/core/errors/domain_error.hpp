#pragma once

#include <cstdint>
#include <string_view>

namespace core {

enum class DomainError : std::uint8_t {
    InvalidTransition,
    AlreadyTerminal,
    EmptyIdentifier,
    MalformedIdentifier,
    InvalidStorageKey,
    InvalidContentType,
    InvalidTitle,
    MissingFailureReason,
    InvalidDuration,
    CorruptRecord,
    InvalidUploadSize,
    InvalidChunkSize,
    OffsetRegression,
    OffsetBeyondSize,
    UploadNotActive,
    UploadIncomplete,
    InvalidFailureReason,
};

[[nodiscard]] std::string_view to_string(DomainError e) noexcept;

} // namespace core
