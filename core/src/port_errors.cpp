#include "core/ports/auth.hpp"
#include "core/ports/catalog.hpp"
#include "core/ports/e2ee.hpp"
#include "core/ports/live.hpp"
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

std::string_view to_string(E2eeError e) noexcept {
    switch (e) {
    case E2eeError::NotFound:
        return "device not found";
    case E2eeError::Revoked:
        return "device deregistered";
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

std::string_view to_string(MediaError e) noexcept {
    switch (e) {
    case MediaError::Unavailable:
        return "media server unavailable";
    case MediaError::Refused:
        return "media server refused the request";
    case MediaError::Closed:
        return "media room closed";
    case MediaError::NotImplemented:
        return "media operation not implemented";
    }
    return "unknown media error";
}

std::string_view to_string(LiveState s) noexcept {
    switch (s) {
    case LiveState::Starting:
        return "starting";
    case LiveState::Live:
        return "live";
    case LiveState::Ended:
        return "ended";
    }
    return "ended";
}

std::string_view to_string(LiveEnd e) noexcept {
    switch (e) {
    case LiveEnd::Owner:
        return "owner";
    case LiveEnd::Finished:
        return "finished";
    case LiveEnd::Failed:
        return "failed";
    case LiveEnd::Timeout:
        return "timeout";
    }
    return "failed";
}

std::string_view to_string(LiveStoreError e) noexcept {
    switch (e) {
    case LiveStoreError::NotFound:
        return "not found";
    case LiveStoreError::Full:
        return "too many unfinished streams";
    case LiveStoreError::TooMany:
        return "too many streams created by the owner";
    case LiveStoreError::Unavailable:
        return "stream store unavailable";
    case LiveStoreError::Corrupt:
        return "corrupt stream record";
    }
    return "unknown stream store error";
}

std::string_view to_string(PackagerState s) noexcept {
    switch (s) {
    case PackagerState::Absent:
        return "absent";
    case PackagerState::Starting:
        return "starting";
    case PackagerState::Ready:
        return "ready";
    case PackagerState::Finished:
        return "finished";
    case PackagerState::Failed:
        return "failed";
    }
    return "absent";
}

std::string_view to_string(PackagerError e) noexcept {
    switch (e) {
    case PackagerError::Unavailable:
        return "packager runtime unavailable";
    case PackagerError::Full:
        return "packager runtime at its quota";
    case PackagerError::Refused:
        return "packager runtime refused the request";
    }
    return "unknown packager error";
}

} // namespace core::ports
