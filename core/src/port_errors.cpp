#include "core/ports/auth.hpp"
#include "core/ports/catalog.hpp"
#include "core/ports/media.hpp"

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

std::string_view to_string(MediaError e) noexcept {
    switch (e) {
    case MediaError::Unavailable:
        return "media server unavailable";
    case MediaError::Refused:
        return "media server refused the request";
    case MediaError::Closed:
        return "media room closed";
    }
    return "unknown media error";
}

} // namespace core::ports
