#pragma once

#include "net/reactor.hpp"

#include <functional>
#include <utility>

namespace infra::postgres {

// A reactor timer bound to one action and armed at most once at a time.
class Timer final : public net::ITimerHandler {
public:
    Timer(net::IReactor& reactor, std::move_only_function<void() noexcept> action) noexcept
        : reactor_(reactor), action_(std::move(action)) {}
    ~Timer() override { cancel(); }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    void arm(core::Millis delay) noexcept {
        cancel();
        id_ = reactor_.arm_timer(delay, *this);
        armed_ = true;
    }
    void arm_unless_armed(core::Millis delay) noexcept {
        if (!armed_) {
            arm(delay);
        }
    }
    void cancel() noexcept {
        if (armed_) {
            reactor_.cancel_timer(id_);
            armed_ = false;
        }
    }

    void on_timeout() noexcept override {
        armed_ = false;
        action_();
    }

private:
    net::IReactor& reactor_;
    std::move_only_function<void() noexcept> action_;
    net::TimerId id_{};
    bool armed_ = false;
};

} // namespace infra::postgres
