#include "infra/e2ee/memory_directory.hpp"
#include "net/reactor_factory.hpp"

#include "e2ee_directory_contract.hpp"
#include "support/fake_clock.hpp"

#include <gtest/gtest.h>
#include <memory>

namespace {

using ulw::test::DirectoryContract;
using ulw::test::DirectoryFactory;
using ulw::test::DirectoryHarness;

class MemoryHarness final : public DirectoryHarness {
public:
    MemoryHarness() {
        auto r = net::make_reactor_with_fallback(net::ReactorKind::IoUring, clock_, 256);
        if (!r) {
            ADD_FAILURE() << "reactor: " << r.error();
            return;
        }
        reactor_ = std::move(r->reactor);
        directory_ = std::make_unique<infra::e2ee::MemoryDirectory>(*reactor_);
    }
    ~MemoryHarness() override {
        // Before the reactor it arms its timer on.
        directory_.reset();
    }
    MemoryHarness(const MemoryHarness&) = delete;
    MemoryHarness& operator=(const MemoryHarness&) = delete;
    MemoryHarness(MemoryHarness&&) = delete;
    MemoryHarness& operator=(MemoryHarness&&) = delete;

    [[nodiscard]] bool ready() const noexcept { return directory_ != nullptr; }

    net::IReactor& reactor() override { return *reactor_; }
    core::ports::IDeviceRegistry& registry() override { return *directory_; }
    core::ports::IE2eeDeliveryService& delivery() override { return *directory_; }

private:
    ulw::test::FakeClock clock_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::e2ee::MemoryDirectory> directory_;
};

INSTANTIATE_TEST_SUITE_P(Memory, DirectoryContract,
                         ::testing::Values(DirectoryFactory{
                             .name = "memory",
                             .make = []() -> std::unique_ptr<DirectoryHarness> {
                                 auto harness = std::make_unique<MemoryHarness>();
                                 if (!harness->ready()) {
                                     return nullptr;
                                 }
                                 return harness;
                             }}),
                         [](const auto& p) { return p.param.name; });

} // namespace
