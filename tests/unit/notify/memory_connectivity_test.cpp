#include "infra/notify/memory_connectivity.hpp"
#include "net/reactor_factory.hpp"

#include "notify_store_contract.hpp"
#include "support/fake_clock.hpp"

#include <gtest/gtest.h>
#include <memory>

namespace {

using ulw::test::NotifyFactory;
using ulw::test::NotifyHarness;
using ulw::test::NotifyStoreContract;

class MemoryHarness final : public NotifyHarness {
public:
    MemoryHarness() {
        auto r = net::make_reactor_with_fallback(net::ReactorKind::IoUring, reactor_clock_, 256);
        if (!r) {
            ADD_FAILURE() << "reactor: " << r.error();
            return;
        }
        reactor_ = std::move(r->reactor);
        store_ = std::make_unique<infra::notify::MemoryConnectivity>(*reactor_, clock_,
                                                                     ulw::test::kContractTiming);
    }
    ~MemoryHarness() override {
        // Before the reactor it arms its timer on.
        store_.reset();
    }
    MemoryHarness(const MemoryHarness&) = delete;
    MemoryHarness& operator=(const MemoryHarness&) = delete;
    MemoryHarness(MemoryHarness&&) = delete;
    MemoryHarness& operator=(MemoryHarness&&) = delete;

    [[nodiscard]] bool ready() const noexcept { return store_ != nullptr; }

    net::IReactor& reactor() override { return *reactor_; }
    core::ports::IConnectivityStore& store() override { return *store_; }
    void set_room(const core::RoomId& room, core::ports::RoomKind kind,
                  const std::vector<core::UserId>& members) override {
        store_->set_room(room, kind, members);
    }
    void wait(core::Millis d) override { clock_.advance(d); }

private:
    // The store's time, which wait() moves; the reactor keeps its own.
    ulw::test::FakeClock clock_;
    ulw::test::FakeClock reactor_clock_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<infra::notify::MemoryConnectivity> store_;
};

INSTANTIATE_TEST_SUITE_P(Memory, NotifyStoreContract,
                         ::testing::Values(NotifyFactory{
                             .name = "memory",
                             .make = []() -> std::unique_ptr<NotifyHarness> {
                                 auto harness = std::make_unique<MemoryHarness>();
                                 if (!harness->ready()) {
                                     return nullptr;
                                 }
                                 return harness;
                             }}),
                         [](const auto& p) { return p.param.name; });

TEST(MemoryConnectivityTest, AFailingStoreAnswersUnavailable) {
    ulw::test::FakeClock clock;
    auto r = net::make_reactor_with_fallback(net::ReactorKind::IoUring, clock, 64);
    ASSERT_TRUE(r);
    infra::notify::MemoryConnectivity store(*r->reactor, clock, ulw::test::kContractTiming);
    store.fail(true);
    std::optional<core::ports::NotifyResult<bool>> got;
    store.renew(1, [&](core::ports::NotifyResult<bool> result) noexcept { got = result; });
    ASSERT_TRUE(ulw::test::pump_until(
        *r->reactor, [&] { return got.has_value(); }, std::chrono::seconds(5)));
    EXPECT_EQ(got->error(), core::ports::NotifyError::Unavailable);
}

} // namespace
