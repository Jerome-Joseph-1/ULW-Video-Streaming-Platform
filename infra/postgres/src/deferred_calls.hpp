#pragma once

#include "net/reactor.hpp"

#include "timer.hpp"

#include <functional>
#include <utility>
#include <vector>

namespace infra::postgres {

// Runs calls on a later loop iteration. For results known before the call that asked for them
// has returned, which a callback must never see from inside that call.
class DeferredCalls {
public:
    explicit DeferredCalls(net::IReactor& reactor)
        : timer_(reactor, [this]() noexcept { run(); }) {}

    void post(std::move_only_function<void() noexcept> call) {
        pending_.push_back(std::move(call));
        timer_.arm_unless_armed(core::Millis{0});
    }

private:
    void run() noexcept {
        // Calls posted from these land in pending_ and wait for the next iteration.
        std::vector<std::move_only_function<void() noexcept>> due;
        due.swap(pending_);
        for (auto& call : due) {
            call();
        }
    }

    std::vector<std::move_only_function<void() noexcept>> pending_;
    Timer timer_;
};

} // namespace infra::postgres
