#pragma once

#include "core/ports/e2ee.hpp"
#include "net/reactor.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

namespace infra::e2ee {

// The device registry and key package directory without a database, for tests and
// single-process development. Same contract as the durable one: results arrive on a later loop
// iteration, and a package is handed out at most once.
class MemoryDirectory final : public core::ports::IDeviceRegistry,
                              public core::ports::IE2eeDeliveryService,
                              public net::ITimerHandler {
public:
    explicit MemoryDirectory(net::IReactor& reactor);
    ~MemoryDirectory() override;
    MemoryDirectory(const MemoryDirectory&) = delete;
    MemoryDirectory& operator=(const MemoryDirectory&) = delete;
    MemoryDirectory(MemoryDirectory&&) = delete;
    MemoryDirectory& operator=(MemoryDirectory&&) = delete;

    void register_device(const core::UserId& user, const core::DeviceId& device,
                         core::ports::E2eeCallback<void> done) override;
    void deregister_device(const core::UserId& user, const core::DeviceId& device,
                           core::ports::E2eeCallback<void> done) override;
    void publish_key_packages(const core::UserId& user, const core::DeviceId& device,
                              std::vector<core::ports::KeyPackageBytes> batch,
                              core::ports::E2eeCallback<std::size_t> done) override;
    void fetch_key_package(const core::UserId& user, const core::DeviceId& device,
                           core::ports::E2eeCallback<core::ports::FetchedKeyPackage> done) override;
    void submit_commit(const core::RoomId& room, const core::UserId& user,
                       const core::DeviceId& committer, std::uint64_t epoch,
                       core::ports::E2eeCallback<void> done) override;

    void on_timeout() noexcept override;

private:
    struct Device {
        core::UserId user;
        bool revoked = false;
        std::deque<core::ports::KeyPackageBytes> packages;
    };

    // The device if `user` owns it and it is live; NotFound or Revoked otherwise.
    [[nodiscard]] core::ports::E2eeResult<Device*> live_device(const core::UserId& user,
                                                               const core::DeviceId& device);
    template <class T> void reply(core::ports::E2eeCallback<T> done, core::ports::E2eeResult<T> r);
    void defer(std::move_only_function<void() noexcept> fn);

    net::IReactor& reactor_;
    net::TimerId timer_;
    std::vector<std::move_only_function<void() noexcept>> pending_;
    std::unordered_map<core::DeviceId, Device> devices_;
    // The epoch each room's next commit must have been built at.
    std::unordered_map<core::RoomId, std::uint64_t> next_epoch_;
};

} // namespace infra::e2ee
