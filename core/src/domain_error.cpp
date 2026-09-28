#include "core/errors/domain_error.hpp"

namespace core {

std::string_view to_string(DomainError e) noexcept {
    switch (e) {
    case DomainError::InvalidTransition:
        return "invalid state transition";
    case DomainError::AlreadyTerminal:
        return "already in a terminal state";
    case DomainError::EmptyIdentifier:
        return "empty identifier";
    case DomainError::MalformedIdentifier:
        return "malformed identifier";
    case DomainError::InvalidStorageKey:
        return "invalid storage key";
    case DomainError::InvalidContentType:
        return "invalid content type";
    }
    return "unknown domain error";
}

} // namespace core
