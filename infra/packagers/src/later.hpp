#pragma once

#include "net/reactor.hpp"

#include <functional>
#include <utility>
#include <vector>

namespace infra::packagers::detail {

// Runs calls on a later loop iteration: for answers known before the call that asked for them
// has returned, which a callback must never see from inside that call. Calls still queued when
// this is destroyed never run.
class Later final : public net::ITimerHandler {
public:
    explicit Later(net::IReactor& reactor) noexcept : reactor_(reactor) {}
    ~Later() override {
        if (armed_) {
            reactor_.cancel_timer(timer_);
        }
    }
    Later(const Later&) = delete;
    Later& operator=(const Later&) = delete;
    Later(Later&&) = delete;
    Later& operator=(Later&&) = delete;

    void post(std::move_only_function<void() noexcept> call) {
        pending_.push_back(std::move(call));
        if (!armed_) {
            timer_ = reactor_.arm_timer(core::Millis{0}, *this);
            armed_ = true;
        }
    }

    void on_timeout() noexcept override {
        armed_ = false;
        // Calls posted from these wait for the next iteration.
        std::vector<std::move_only_function<void() noexcept>> due;
        due.swap(pending_);
        for (auto& call : due) {
            call();
        }
    }

private:
    net::IReactor& reactor_;
    std::vector<std::move_only_function<void() noexcept>> pending_;
    net::TimerId timer_{};
    bool armed_ = false;
};

} // namespace infra::packagers::detail
