#include "support/fake_clock.hpp"
#include "timing_wheel.hpp"

#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace {

using core::Millis;
using net::detail::TimingWheel;

struct Recorder final : net::ITimerHandler {
    std::vector<int>* log = nullptr;
    int id = 0;
    int fired = 0;
    void on_timeout() noexcept override {
        ++fired;
        if (log != nullptr) {
            log->push_back(id);
        }
    }
};

class TimingWheelTest : public ::testing::Test {
protected:
    ulw::test::FakeClock clock;
    TimingWheel wheel{clock.now()};

    void advance(Millis d) {
        clock.advance(d);
        wheel.tick_to(clock.now());
    }
};

TEST_F(TimingWheelTest, NeverFiresBeforeTheDeadline) {
    Recorder r;
    wheel.arm(clock.now(), Millis{250}, r);
    advance(Millis{249});
    EXPECT_EQ(r.fired, 0);
    advance(Millis{100});
    EXPECT_EQ(r.fired, 1);
}

TEST_F(TimingWheelTest, FiresAtMostOneTickLate) {
    Recorder r;
    wheel.arm(clock.now(), Millis{1}, r);
    advance(TimingWheel::kTick);
    EXPECT_EQ(r.fired, 1);
}

TEST_F(TimingWheelTest, FiresInDeadlineOrderAcrossTicks) {
    std::vector<int> log;
    std::array<Recorder, 3> r;
    for (int i = 0; i < 3; ++i) {
        r[static_cast<std::size_t>(i)].log = &log;
        r[static_cast<std::size_t>(i)].id = i + 1;
    }
    auto& [a, b, c] = r;
    wheel.arm(clock.now(), Millis{900}, c);
    wheel.arm(clock.now(), Millis{100}, a);
    wheel.arm(clock.now(), Millis{500}, b);
    for (int i = 0; i < 10; ++i) {
        advance(Millis{100});
    }
    EXPECT_EQ(log, (std::vector<int>{1, 2, 3}));
}

TEST_F(TimingWheelTest, CancelledTimerNeverFiresAndStaleCancelIsHarmless) {
    Recorder r;
    const auto id = wheel.arm(clock.now(), Millis{100}, r);
    wheel.cancel(id);
    advance(Millis{1000});
    EXPECT_EQ(r.fired, 0);

    Recorder other;
    const auto reused = wheel.arm(clock.now(), Millis{100}, other);
    EXPECT_EQ(reused.index, id.index);
    wheel.cancel(id);
    advance(Millis{200});
    EXPECT_EQ(other.fired, 1);
}

TEST_F(TimingWheelTest, TimerLongerThanOneRevolutionWaitsForItsDeadline) {
    Recorder r;
    wheel.arm(clock.now(), std::chrono::duration_cast<Millis>(std::chrono::hours(6)), r);
    for (int i = 0; i < 6 * 36000 - 1; ++i) {
        advance(Millis{100});
    }
    EXPECT_EQ(r.fired, 0);
    advance(Millis{100});
    EXPECT_EQ(r.fired, 1);
}

TEST_F(TimingWheelTest, CatchesUpAfterAStallLongerThanARevolution) {
    Recorder due;
    Recorder later;
    wheel.arm(clock.now(), Millis{5'000}, due);
    wheel.arm(clock.now(), Millis{120'000}, later);
    advance(Millis{100'000});
    EXPECT_EQ(due.fired, 1);
    EXPECT_EQ(later.fired, 0);
    advance(Millis{20'000});
    EXPECT_EQ(later.fired, 1);
}

struct Rearmer final : net::ITimerHandler {
    TimingWheel* wheel = nullptr;
    ulw::test::FakeClock* clock = nullptr;
    int fired = 0;
    void on_timeout() noexcept override {
        if (++fired < 3) {
            wheel->arm(clock->now(), Millis{0}, *this);
        }
    }
};

TEST_F(TimingWheelTest, HandlerMayRearmItselfWithoutBeingFiredTwiceInOnePass) {
    Rearmer r;
    r.wheel = &wheel;
    r.clock = &clock;
    wheel.arm(clock.now(), Millis{0}, r);
    advance(Millis{100});
    EXPECT_EQ(r.fired, 1);
    advance(Millis{100});
    advance(Millis{100});
    EXPECT_EQ(r.fired, 3);
    EXPECT_EQ(wheel.armed(), 0U);
}

TEST_F(TimingWheelTest, NextExpiryReportsTheNearestOccupiedTick) {
    EXPECT_FALSE(wheel.next_expiry(clock.now()).has_value());
    Recorder r;
    wheel.arm(clock.now(), Millis{350}, r);
    const auto next = wheel.next_expiry(clock.now());
    ASSERT_TRUE(next.has_value());
    EXPECT_GE(*next, Millis{350});
    EXPECT_LE(*next, Millis{400});
}

} // namespace
