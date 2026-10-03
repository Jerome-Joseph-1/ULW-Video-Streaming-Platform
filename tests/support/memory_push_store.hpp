#pragma once

#include "core/ports/push_subscriptions.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ulw::test {

// IPushSubscriptions in memory, with the Postgres store's rules (an endpoint belongs to one
// device of one user; past the cap the least recently saved go). Answers wait until flush(), so
// a test, or a test node's loop, decides when the store has answered.
class MemoryPushStore final : public core::ports::IPushSubscriptions {
public:
    struct Row {
        core::UserId user;
        core::ports::PushSubscription subscription;
        std::uint64_t saved = 0;
    };

    void save(const core::UserId& user, const core::ports::PushSubscription& subscription,
              std::size_t max_per_user, core::ports::PushCallback<void> done) override {
        ++saves;
        later([this, user, subscription, max_per_user, done = std::move(done)]() mutable noexcept {
            if (fail) {
                done(std::unexpected(core::ports::PushStoreError::Unavailable));
                return;
            }
            std::erase_if(rows, [&](const Row& r) {
                return r.subscription.endpoint == subscription.endpoint ||
                       (r.user == user && r.subscription.device == subscription.device);
            });
            rows.push_back(Row{.user = user, .subscription = subscription, .saved = ++clock});
            std::vector<Row*> mine;
            for (Row& r : rows) {
                if (r.user == user) {
                    mine.push_back(&r);
                }
            }
            if (mine.size() > max_per_user) {
                std::ranges::sort(mine, std::greater{}, &Row::saved);
                const std::uint64_t oldest_kept = mine[max_per_user - 1]->saved;
                std::erase_if(
                    rows, [&](const Row& r) { return r.user == user && r.saved < oldest_kept; });
            }
            done({});
        });
    }

    void remove(const core::UserId& user, const core::DeviceId& device,
                core::ports::PushCallback<void> done) override {
        later([this, user, device, done = std::move(done)]() mutable noexcept {
            if (fail) {
                done(std::unexpected(core::ports::PushStoreError::Unavailable));
                return;
            }
            std::erase_if(rows, [&](const Row& r) {
                return r.user == user && r.subscription.device == device;
            });
            done({});
        });
    }

    void list(const core::UserId& user, std::size_t limit,
              core::ports::PushCallback<std::vector<core::ports::PushSubscription>> done) override {
        ++lists;
        later([this, user, limit, done = std::move(done)]() mutable noexcept {
            if (fail) {
                done(std::unexpected(core::ports::PushStoreError::Unavailable));
                return;
            }
            std::vector<const Row*> mine;
            for (const Row& r : rows) {
                if (r.user == user) {
                    mine.push_back(&r);
                }
            }
            std::ranges::sort(mine, std::greater{}, &Row::saved);
            std::vector<core::ports::PushSubscription> out;
            for (const Row* r : mine) {
                if (out.size() < limit) {
                    out.push_back(r->subscription);
                }
            }
            done(std::move(out));
        });
    }

    void forget(const std::string& endpoint, core::ports::PushCallback<void> done) override {
        later([this, endpoint, done = std::move(done)]() mutable noexcept {
            if (fail) {
                done(std::unexpected(core::ports::PushStoreError::Unavailable));
                return;
            }
            std::erase_if(rows, [&](const Row& r) { return r.subscription.endpoint == endpoint; });
            done({});
        });
    }

    // Answers everything asked so far, and whatever those answers ask in turn.
    void flush() {
        while (!pending_.empty()) {
            auto batch = std::move(pending_);
            pending_.clear();
            for (auto& fn : batch) {
                fn();
            }
        }
    }
    [[nodiscard]] std::size_t waiting() const noexcept { return pending_.size(); }

    std::vector<Row> rows;
    bool fail = false;
    std::size_t saves = 0;
    std::size_t lists = 0;

private:
    void later(std::move_only_function<void() noexcept> fn) { pending_.push_back(std::move(fn)); }

    std::uint64_t clock = 0;
    std::vector<std::move_only_function<void() noexcept>> pending_;
};

} // namespace ulw::test
