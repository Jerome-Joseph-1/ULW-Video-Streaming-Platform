#include "core/ports/storage.hpp"

#include <string_view>

namespace core::ports {

std::string_view to_string(StorageError e) noexcept {
    switch (e) {
    case StorageError::NotFound:
        return "not found";
    case StorageError::AlreadyExists:
        return "already exists";
    case StorageError::PreconditionFailed:
        return "precondition failed";
    case StorageError::Unauthorized:
        return "unauthorized";
    case StorageError::Throttled:
        return "throttled";
    case StorageError::Transient:
        return "transient backend error";
    case StorageError::Permanent:
        return "permanent backend error";
    case StorageError::Corrupt:
        return "corrupt data";
    }
    return "unknown storage error";
}

} // namespace core::ports
