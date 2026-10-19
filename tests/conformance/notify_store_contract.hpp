#pragma once

#include "core/ports/message_store.hpp"
#include "core/ports/notify.hpp"
#include "net/reactor.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace ulw::test {

// The timing every connectivity store is tested with: short, so that a lease and an interval
// pass in real time against a database whose clock no test can move.
inline constexpr core::ports::NotifyTiming kContractTiming{.lease = core::Millis{600},
                                                           .message_interval = core::Millis{900},
                                                           .forget_after = core::Millis{1'500}};

// One connectivity store plus the loop that drives it, and the rooms its message_targets reads.
class NotifyHarness {
public:
    NotifyHarness() = default;
    NotifyHarness(const NotifyHarness&) = delete;
    NotifyHarness& operator=(const NotifyHarness&) = delete;
    virtual ~NotifyHarness() = default;

    virtual net::IReactor& reactor() = 0;
    virtual core::ports::IConnectivityStore& store() = 0;
    // A room of `kind` listing `members`, as the message store would have it.
    virtual void set_room(const core::RoomId& room, core::ports::RoomKind kind,
                          const std::vector<core::UserId>& members) = 0;
    // Lets `d` pass for the store: a fake clock moves, a database's is waited out.
    virtual void wait(core::Millis d) = 0;
};

struct NotifyFactory {
    std::string name;
    // May skip the calling test (no database) and return null.
    std::function<std::unique_ptr<NotifyHarness>()> make;
    friend void PrintTo(const NotifyFactory& f, std::ostream* os) { *os << f.name; }
};

// The behaviour every connectivity store shares, run against each implementation.
class NotifyStoreContract : public ::testing::TestWithParam<NotifyFactory> {
protected:
    void SetUp() override {
        harness_ = GetParam().make();
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        ASSERT_NE(harness_, nullptr);
    }

    template <class T, class Start> core::ports::NotifyResult<T> call(Start start) {
        std::optional<core::ports::NotifyResult<T>> got;
        start([&](core::ports::NotifyResult<T> r) noexcept { got = std::move(r); });
        if (!pump_until(
                harness_->reactor(), [&] { return got.has_value(); }, std::chrono::seconds(15))) {
            ADD_FAILURE() << "store call never answered";
            return std::unexpected(core::ports::NotifyError::Unavailable);
        }
        return std::move(*got);
    }

    core::ports::NotifyResult<bool> renew(std::uint64_t node) {
        return call<bool>([&](auto done) { harness_->store().renew(node, std::move(done)); });
    }
    core::ports::NotifyResult<void> sync(std::uint64_t node,
                                         std::vector<core::ports::SocketDevice> added,
                                         std::vector<core::ports::SocketDevice> removed = {}) {
        return call<void>([&](auto done) {
            harness_->store().sync(node, std::move(added), std::move(removed), std::move(done));
        });
    }
    core::ports::NotifyResult<void> forget(std::uint64_t node) {
        return call<void>([&](auto done) { harness_->store().forget(node, std::move(done)); });
    }
    core::ports::NotifyResult<std::vector<core::ports::SocketDevice>>
    connected(std::vector<core::UserId> users) {
        return call<std::vector<core::ports::SocketDevice>>(
            [&](auto done) { harness_->store().connected(std::move(users), std::move(done)); });
    }
    core::ports::NotifyResult<std::vector<core::UserId>> targets(const core::RoomId& room,
                                                                 const core::UserId& sender) {
        return call<std::vector<core::UserId>>(
            [&](auto done) { harness_->store().message_targets(room, sender, std::move(done)); });
    }

    core::ports::NotifyResult<core::ports::MessageAudience> audience(const core::RoomId& room,
                                                                     const core::UserId& sender) {
        return call<core::ports::MessageAudience>(
            [&](auto done) { harness_->store().message_audience(room, sender, std::move(done)); });
    }

    core::DeviceId new_device() { return core::DeviceId::generate(clock_, random_); }
    core::RoomId new_room() { return core::RoomId::generate(clock_, random_); }

    FakeClock clock_;
    FakeRandom random_;
    std::unique_ptr<NotifyHarness> harness_;
    const core::UserId alice = *core::UserId::parse("auth0|alice");
    const core::UserId bob = *core::UserId::parse("auth0|bob");
    const core::UserId carol = *core::UserId::parse("auth0|carol");
};

} // namespace ulw::test
