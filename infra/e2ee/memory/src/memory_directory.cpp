#include "infra/e2ee/memory_directory.hpp"

#include <utility>

namespace infra::e2ee {

using core::ports::E2eeCallback;
using core::ports::E2eeError;
using core::ports::E2eeResult;
using core::ports::FetchedKeyPackage;
using core::ports::KeyPackageBytes;

MemoryDirectory::MemoryDirectory(net::IReactor& reactor) : reactor_(reactor) {}

MemoryDirectory::~MemoryDirectory() {
    reactor_.cancel_timer(timer_);
}

void MemoryDirectory::defer(std::move_only_function<void() noexcept> fn) {
    pending_.push_back(std::move(fn));
    if (timer_ == net::TimerId{}) {
        timer_ = reactor_.arm_timer(core::Millis{0}, *this);
    }
}

void MemoryDirectory::on_timeout() noexcept {
    timer_ = {};
    // Callbacks may issue further calls; those land in a fresh batch for the next iteration.
    std::vector<std::move_only_function<void() noexcept>> batch;
    batch.swap(pending_);
    for (auto& fn : batch) {
        fn();
    }
}

template <class T> void MemoryDirectory::reply(E2eeCallback<T> done, E2eeResult<T> r) {
    defer([done = std::move(done), r = std::move(r)]() mutable noexcept { done(std::move(r)); });
}

E2eeResult<MemoryDirectory::Device*> MemoryDirectory::live_device(const core::UserId& user,
                                                                  const core::DeviceId& device) {
    const auto it = devices_.find(device);
    if (it == devices_.end() || it->second.user != user) {
        return std::unexpected(E2eeError::NotFound);
    }
    if (it->second.revoked) {
        return std::unexpected(E2eeError::Revoked);
    }
    return &it->second;
}

void MemoryDirectory::register_device(const core::UserId& user, const core::DeviceId& device,
                                      E2eeCallback<void> done) {
    const auto [it, inserted] =
        devices_.try_emplace(device, Device{.user = user, .revoked = false, .packages = {}});
    E2eeResult<void> result;
    if (!inserted && it->second.user != user) {
        result = std::unexpected(E2eeError::Conflict);
    } else if (it->second.revoked) {
        result = std::unexpected(E2eeError::Revoked);
    }
    reply(std::move(done), std::move(result));
}

void MemoryDirectory::deregister_device(const core::UserId& user, const core::DeviceId& device,
                                        E2eeCallback<void> done) {
    const auto it = devices_.find(device);
    if (it == devices_.end() || it->second.user != user) {
        reply<void>(std::move(done), std::unexpected(E2eeError::NotFound));
        return;
    }
    it->second.revoked = true;
    it->second.packages.clear();
    reply<void>(std::move(done), {});
}

void MemoryDirectory::publish_key_packages(const core::UserId& user, const core::DeviceId& device,
                                           std::vector<KeyPackageBytes> batch,
                                           E2eeCallback<std::size_t> done) {
    if (auto checked = core::ports::check_key_package_batch(batch); !checked) {
        reply<std::size_t>(std::move(done), std::unexpected(checked.error()));
        return;
    }
    auto live = live_device(user, device);
    if (!live) {
        reply<std::size_t>(std::move(done), std::unexpected(live.error()));
        return;
    }
    auto& packages = (*live)->packages;
    if (packages.size() + batch.size() > core::ports::kMaxKeyPackagesPerDevice) {
        reply<std::size_t>(std::move(done), std::unexpected(E2eeError::Full));
        return;
    }
    for (KeyPackageBytes& package : batch) {
        packages.push_back(std::move(package));
    }
    reply<std::size_t>(std::move(done), packages.size());
}

void MemoryDirectory::fetch_key_package(const core::UserId& user, const core::DeviceId& device,
                                        E2eeCallback<FetchedKeyPackage> done) {
    auto live = live_device(user, device);
    if (!live) {
        reply<FetchedKeyPackage>(std::move(done), std::unexpected(live.error()));
        return;
    }
    auto& packages = (*live)->packages;
    if (packages.empty()) {
        reply<FetchedKeyPackage>(std::move(done), std::unexpected(E2eeError::Exhausted));
        return;
    }
    FetchedKeyPackage fetched{.package = std::move(packages.front()), .replenish = false};
    packages.pop_front();
    fetched.replenish = packages.size() <= core::ports::kKeyPackageLowWater;
    reply<FetchedKeyPackage>(std::move(done), std::move(fetched));
}

void MemoryDirectory::submit_commit(const core::RoomId& room, const core::UserId& user,
                                    const core::DeviceId& committer, std::uint64_t epoch,
                                    E2eeCallback<void> done) {
    if (auto live = live_device(user, committer); !live) {
        reply<void>(std::move(done), std::unexpected(live.error()));
        return;
    }
    std::uint64_t& next = next_epoch_[room];
    if (epoch != next) {
        reply<void>(std::move(done), std::unexpected(E2eeError::StaleEpoch));
        return;
    }
    ++next;
    reply<void>(std::move(done), {});
}

} // namespace infra::e2ee
