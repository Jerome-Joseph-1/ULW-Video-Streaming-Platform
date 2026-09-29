#pragma once

#include "core/ports/e2ee.hpp"
#include "net/reactor.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"

#include <chrono>
#include <cstddef>
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

// One directory implementation plus the loop that drives it.
class DirectoryHarness {
public:
    DirectoryHarness() = default;
    DirectoryHarness(const DirectoryHarness&) = delete;
    DirectoryHarness& operator=(const DirectoryHarness&) = delete;
    virtual ~DirectoryHarness() = default;

    virtual net::IReactor& reactor() = 0;
    virtual core::ports::IDeviceRegistry& registry() = 0;
    virtual core::ports::IE2eeDeliveryService& delivery() = 0;
};

struct DirectoryFactory {
    std::string name;
    // May skip the calling test (no database) and return null.
    std::function<std::unique_ptr<DirectoryHarness>()> make;
    friend void PrintTo(const DirectoryFactory& f, std::ostream* os) { *os << f.name; }
};

// The result a directory call delivers, once the reactor has delivered it.
template <class T> class Answer {
public:
    [[nodiscard]] core::ports::E2eeCallback<T> callback() {
        return [this](core::ports::E2eeResult<T> r) noexcept {
            ++calls_;
            result_ = std::move(r);
        };
    }
    [[nodiscard]] bool ready() const noexcept { return result_.has_value(); }
    [[nodiscard]] int calls() const noexcept { return calls_; }
    [[nodiscard]] const core::ports::E2eeResult<T>& get() const { return *result_; }

private:
    std::optional<core::ports::E2eeResult<T>> result_;
    int calls_ = 0;
};

template <class T>
core::ports::E2eeResult<T> await(net::IReactor& reactor, Answer<T>& answer,
                                 std::chrono::milliseconds limit = std::chrono::seconds(15)) {
    if (!pump_until(reactor, [&] { return answer.ready(); }, limit)) {
        ADD_FAILURE() << "directory call never answered";
        return std::unexpected(core::ports::E2eeError::Unavailable);
    }
    return answer.get();
}

// A recognisable package: `size` bytes counting up from `seed`.
[[nodiscard]] core::ports::KeyPackageBytes package_of(std::size_t size, std::uint8_t seed);

// The behaviour every directory shares, run against each implementation; the executable that
// links this instantiates it with its factories.
class DirectoryContract : public ::testing::TestWithParam<DirectoryFactory> {
protected:
    void SetUp() override {
        harness_ = GetParam().make();
        if (IsSkipped() || HasFatalFailure()) {
            return;
        }
        ASSERT_NE(harness_, nullptr);
    }

    net::IReactor& reactor() { return harness_->reactor(); }
    core::ports::IDeviceRegistry& registry() { return harness_->registry(); }
    core::ports::IE2eeDeliveryService& delivery() { return harness_->delivery(); }

    core::DeviceId new_device() { return core::DeviceId::generate(clock_, random_); }
    core::RoomId new_room() { return core::RoomId::generate(clock_, random_); }

    template <class T, class Start> core::ports::E2eeResult<T> call(Start start) {
        Answer<T> answer;
        start(answer.callback());
        return await(harness_->reactor(), answer);
    }

    core::ports::E2eeResult<void> enrol(const core::UserId& user, const core::DeviceId& device) {
        return call<void>([&](auto done) {
            harness_->registry().register_device(user, device, std::move(done));
        });
    }
    core::ports::E2eeResult<void> retire(const core::UserId& user, const core::DeviceId& device) {
        return call<void>([&](auto done) {
            harness_->registry().deregister_device(user, device, std::move(done));
        });
    }
    core::ports::E2eeResult<std::size_t> publish(const core::UserId& user,
                                                 const core::DeviceId& device,
                                                 std::vector<core::ports::KeyPackageBytes> batch) {
        return call<std::size_t>([&](auto done) {
            harness_->delivery().publish_key_packages(user, device, std::move(batch),
                                                      std::move(done));
        });
    }
    core::ports::E2eeResult<core::ports::FetchedKeyPackage> fetch(const core::UserId& user,
                                                                  const core::DeviceId& device) {
        return call<core::ports::FetchedKeyPackage>([&](auto done) {
            harness_->delivery().fetch_key_package(user, device, std::move(done));
        });
    }
    core::ports::E2eeResult<void> commit(const core::RoomId& room, const core::UserId& user,
                                         const core::DeviceId& device, std::uint64_t epoch) {
        return call<void>([&](auto done) {
            harness_->delivery().submit_commit(room, user, device, epoch, std::move(done));
        });
    }

    // Registers a device of `alice` holding `count` distinct packages.
    core::DeviceId stocked_device(std::size_t count) {
        const core::DeviceId device = new_device();
        EXPECT_TRUE(enrol(alice, device));
        std::vector<core::ports::KeyPackageBytes> batch;
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            batch.push_back(package_of(300, static_cast<std::uint8_t>(i)));
        }
        if (count > 0) {
            EXPECT_EQ(publish(alice, device, batch), count);
        }
        return device;
    }

    const core::UserId alice = *core::UserId::parse("auth0|alice");
    const core::UserId bob = *core::UserId::parse("auth0|bob");

private:
    FakeClock clock_;
    FakeRandom random_;
    std::unique_ptr<DirectoryHarness> harness_;
};

} // namespace ulw::test
