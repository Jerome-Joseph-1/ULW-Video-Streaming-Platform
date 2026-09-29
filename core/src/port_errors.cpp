#include "core/ports/auth.hpp"
#include "core/ports/catalog.hpp"
#include "core/ports/e2ee.hpp"

namespace core::ports {

std::string_view to_string(CatalogError e) noexcept {
    switch (e) {
    case CatalogError::NotFound:
        return "not found";
    case CatalogError::Conflict:
        return "conflict";
    case CatalogError::Unavailable:
        return "catalog unavailable";
    case CatalogError::Corrupt:
        return "corrupt record";
    }
    return "unknown catalog error";
}

std::string_view to_string(AuthError e) noexcept {
    switch (e) {
    case AuthError::Malformed:
        return "malformed token";
    case AuthError::UnsupportedAlgorithm:
        return "unsupported algorithm";
    case AuthError::UnknownKey:
        return "unknown key";
    case AuthError::BadSignature:
        return "bad signature";
    case AuthError::Expired:
        return "token expired";
    case AuthError::NotYetValid:
        return "token not yet valid";
    case AuthError::WrongIssuer:
        return "wrong issuer";
    case AuthError::WrongAudience:
        return "wrong audience";
    case AuthError::MissingSubject:
        return "missing subject";
    case AuthError::KeysUnavailable:
        return "signing keys unavailable";
    }
    return "unknown auth error";
}

std::string_view to_string(E2eeError e) noexcept {
    switch (e) {
    case E2eeError::NotFound:
        return "device not found";
    case E2eeError::Revoked:
        return "device deregistered";
    case E2eeError::Conflict:
        return "device registered to another user";
    case E2eeError::Exhausted:
        return "no key package left";
    case E2eeError::Full:
        return "key package supply full";
    case E2eeError::Invalid:
        return "invalid key package batch";
    case E2eeError::StaleEpoch:
        return "stale epoch";
    case E2eeError::Unavailable:
        return "directory unavailable";
    case E2eeError::Corrupt:
        return "corrupt record";
    }
    return "unknown e2ee error";
}

} // namespace core::ports
