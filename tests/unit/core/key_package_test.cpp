#include "core/ports/e2ee.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace {

using core::ports::check_key_package_batch;
using core::ports::E2eeError;
using core::ports::KeyPackageBytes;
using core::ports::kMaxKeyPackageBytes;
using core::ports::kMaxKeyPackagesPerDevice;

TEST(KeyPackageBatch, AcceptsPackagesUpToTheSizeBound) {
    const std::vector<KeyPackageBytes> batch{KeyPackageBytes(1),
                                             KeyPackageBytes(kMaxKeyPackageBytes)};
    EXPECT_TRUE(check_key_package_batch(batch));
}

TEST(KeyPackageBatch, RefusesAnEmptyOrOversizedPackage) {
    const std::vector<KeyPackageBytes> empty{KeyPackageBytes(300), KeyPackageBytes{}};
    EXPECT_EQ(check_key_package_batch(empty).error(), E2eeError::Invalid);
    const std::vector<KeyPackageBytes> oversized{KeyPackageBytes(kMaxKeyPackageBytes + 1)};
    EXPECT_EQ(check_key_package_batch(oversized).error(), E2eeError::Invalid);
}

TEST(KeyPackageBatch, RefusesAnEmptyBatchAndOneNoDeviceCouldHold) {
    EXPECT_EQ(check_key_package_batch({}).error(), E2eeError::Invalid);
    const std::vector<KeyPackageBytes> full(kMaxKeyPackagesPerDevice, KeyPackageBytes(300));
    EXPECT_TRUE(check_key_package_batch(full));
    const std::vector<KeyPackageBytes> over(kMaxKeyPackagesPerDevice + 1, KeyPackageBytes(300));
    EXPECT_EQ(check_key_package_batch(over).error(), E2eeError::Invalid);
}

} // namespace
