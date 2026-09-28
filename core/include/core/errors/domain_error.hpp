#pragma once

#include <string_view>

namespace core {

enum class DomainError {
    InvalidTransition,
    AlreadyTerminal,
    EmptyIdentifier,
    MalformedIdentifier,
    InvalidStorageKey,
    InvalidContentType,
};

[[nodiscard]] std::string_view to_string(DomainError e) noexcept;

} // namespace core
