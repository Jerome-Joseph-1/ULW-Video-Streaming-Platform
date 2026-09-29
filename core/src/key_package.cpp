#include "core/ports/e2ee.hpp"

namespace core::ports {

E2eeResult<void> check_key_package_batch(std::span<const KeyPackageBytes> batch) {
    if (batch.empty() || batch.size() > kMaxKeyPackagesPerDevice) {
        return std::unexpected(E2eeError::Invalid);
    }
    for (const KeyPackageBytes& package : batch) {
        if (package.empty() || package.size() > kMaxKeyPackageBytes) {
            return std::unexpected(E2eeError::Invalid);
        }
    }
    return {};
}

} // namespace core::ports
