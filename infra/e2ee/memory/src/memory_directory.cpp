#include "infra/e2ee/memory_directory.hpp"

#include <algorithm>
#include <utility>

namespace infra::e2ee {

using core::ports::E2eeCallback;
using core::ports::E2eeError;
using core::ports::E2eeResult;
using core::ports::FetchedKeyPackage;
using core::ports::KeyPackageBytes;
using core::ports::StoredCommit;

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

std::size_t MemoryDirectory::live_devices_of(const core::UserId& user) const {
    return static_cast<std::size_t>(std::ranges::count_if(devices_, [&](const auto& entry) {
        return entry.second.user == user && !entry.second.revoked;
    }));
}

void MemoryDirectory::drop_old_tombstones(const core::UserId& user) {
    std::vector<std::pair<std::uint64_t, core::DeviceId>> tombstones;
    for (const auto& [id, d] : devices_) {
        if (d.user == user && d.revoked) {
            tombstones.emplace_back(d.retired_seq, id);
        }
    }
    if (tombstones.size() <= core::ports::kRetiredDevicesKept) {
        return;
    }
    std::ranges::sort(tombstones);
    const std::size_t excess = tombstones.size() - core::ports::kRetiredDevicesKept;
    for (std::size_t i = 0; i < excess; ++i) {
        devices_.erase(tombstones[i].second);
    }
}

void MemoryDirectory::register_device(const core::UserId& user, const core::DeviceId& device,
                                      E2eeCallback<void> done) {
    E2eeResult<void> result;
    if (const auto it = devices_.find(device); it != devices_.end()) {
        if (it->second.user != user) {
            result = std::unexpected(E2eeError::Conflict);
        } else if (it->second.revoked) {
            result = std::unexpected(E2eeError::Revoked);
        }
    } else if (live_devices_of(user) >= core::ports::kMaxDevicesPerUser) {
        result = std::unexpected(E2eeError::Full);
    } else {
        devices_.emplace(device,
                         Device{.user = user, .revoked = false, .retired_seq = 0, .packages = {}});
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
    if (!it->second.revoked) {
        it->second.revoked = true;
        it->second.retired_seq = ++retirements_;
        it->second.packages.clear();
        drop_old_tombstones(user);
    }
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
                                    core::ports::CommitBytes commit, E2eeCallback<void> done) {
    if (commit.empty() || commit.size() > core::ports::kMaxCommitBytes) {
        reply<void>(std::move(done), std::unexpected(E2eeError::Invalid));
        return;
    }
    if (auto live = live_device(user, committer); !live) {
        reply<void>(std::move(done), std::unexpected(live.error()));
        return;
    }
    auto& accepted = commits_[room];
    if (epoch != accepted.size()) {
        reply<void>(std::move(done), std::unexpected(E2eeError::StaleEpoch));
        return;
    }
    accepted.push_back(std::move(commit));
    reply<void>(std::move(done), {});
}

void MemoryDirectory::fetch_commits(const core::RoomId& room, std::uint64_t from_epoch,
                                    E2eeCallback<std::vector<StoredCommit>> done) {
    std::vector<StoredCommit> page;
    if (const auto it = commits_.find(room); it != commits_.end()) {
        for (std::uint64_t e = from_epoch;
             e < it->second.size() && page.size() < core::ports::kCommitPage; ++e) {
            page.push_back(StoredCommit{.epoch = e, .commit = it->second[e]});
        }
    }
    reply<std::vector<StoredCommit>>(std::move(done), std::move(page));
}

} // namespace infra::e2ee
