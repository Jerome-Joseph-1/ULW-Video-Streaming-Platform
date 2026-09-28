#include "timing_wheel.hpp"

#include <algorithm>

namespace net::detail {

TimingWheel::TimingWheel(core::MonoTime now) noexcept : last_tick_(tick_of(now)) {
    heads_.fill(kNil);
}

std::int64_t TimingWheel::tick_of(core::MonoTime t) noexcept {
    return std::chrono::floor<core::Millis>(t.time_since_epoch()).count() / kTick.count();
}

TimerId TimingWheel::arm(core::MonoTime now, core::Millis delay, ITimerHandler& handler) {
    std::uint32_t index = 0;
    if (free_.empty()) {
        index = static_cast<std::uint32_t>(entries_.size());
        entries_.emplace_back();
    } else {
        index = free_.back();
        free_.pop_back();
    }
    Entry& e = entries_[index];
    e.deadline = now + std::max(delay, core::Millis{0});
    e.handler = &handler;
    ++armed_;
    const TimerId id{.index = index, .gen = e.gen};
    if (delay <= core::Millis{0}) {
        e.slot = kImmediate;
        immediate_.push_back(id);
    } else {
        insert(index);
    }
    return id;
}

void TimingWheel::insert(std::uint32_t index) noexcept {
    Entry& e = entries_[index];
    // Round the deadline up to a tick boundary so a timer never fires early, then clamp into
    // the next revolution; a clamped timer finds its deadline unmet and goes round again.
    const std::int64_t due =
        (std::chrono::ceil<core::Millis>(e.deadline.time_since_epoch()) + kTick - core::Millis{1})
            .count() /
        kTick.count();
    const std::int64_t tick =
        std::clamp(due, last_tick_ + 1, last_tick_ + static_cast<std::int64_t>(kSlots) - 1);
    const auto slot = static_cast<std::uint32_t>(tick % static_cast<std::int64_t>(kSlots));
    e.slot = slot;
    e.prev = kNil;
    e.next = head(slot);
    if (e.next != kNil) {
        entries_[e.next].prev = index;
    }
    head(slot) = index;
}

void TimingWheel::unlink(std::uint32_t index) noexcept {
    Entry& e = entries_[index];
    if (e.prev != kNil) {
        entries_[e.prev].next = e.next;
    } else {
        head(e.slot) = e.next;
    }
    if (e.next != kNil) {
        entries_[e.next].prev = e.prev;
    }
    e.prev = e.next = e.slot = kNil;
}

void TimingWheel::release(std::uint32_t index) noexcept {
    Entry& e = entries_[index];
    e.handler = nullptr;
    ++e.gen;
    free_.push_back(index);
    --armed_;
}

void TimingWheel::cancel(TimerId id) noexcept {
    if (id.index >= entries_.size()) {
        return;
    }
    Entry& e = entries_[id.index];
    if (e.gen != id.gen || e.handler == nullptr) {
        return;
    }
    if (e.slot == kImmediate) {
        // Left in immediate_; the generation bump makes tick_to skip it.
        e.slot = kNil;
    } else {
        unlink(id.index);
    }
    release(id.index);
}

std::size_t TimingWheel::tick_to(core::MonoTime now) noexcept {
    std::size_t fired = 0;
    if (!immediate_.empty()) {
        // Swapped out first: handlers that re-arm with zero delay run on the next call.
        std::vector<TimerId> due;
        due.swap(immediate_);
        for (const TimerId id : due) {
            Entry& e = entries_[id.index];
            if (e.gen != id.gen || e.handler == nullptr) {
                continue;
            }
            ITimerHandler* handler = e.handler;
            e.slot = kNil;
            release(id.index);
            handler->on_timeout();
            ++fired;
        }
    }
    const std::int64_t target = tick_of(now);
    // After a stall longer than a revolution every slot has been visited once; stepping
    // further would only revisit them.
    std::int64_t steps = std::min<std::int64_t>(target - last_tick_, kSlots);
    if (target - last_tick_ > steps) {
        last_tick_ = target - steps;
    }
    while (steps-- > 0) {
        ++last_tick_;
        const auto slot = static_cast<std::size_t>(last_tick_ % static_cast<std::int64_t>(kSlots));
        // Take entries off the live bucket one at a time: a handler may cancel any timer,
        // including the next one here, so no link may be held across a call. insert() never
        // targets the slot being fired, which keeps re-armed timers out of this pass.
        while (head(slot) != kNil) {
            const std::uint32_t index = head(slot);
            unlink(index);
            const Entry& e = entries_[index];
            if (e.deadline <= now) {
                ITimerHandler* handler = e.handler;
                release(index);
                handler->on_timeout();
                ++fired;
            } else {
                insert(index);
            }
        }
    }
    return fired;
}

std::optional<core::Millis> TimingWheel::next_expiry(core::MonoTime now) const noexcept {
    if (armed_ == 0) {
        return std::nullopt;
    }
    if (!immediate_.empty()) {
        return core::Millis{0};
    }
    for (std::int64_t tick = last_tick_ + 1; tick < last_tick_ + static_cast<std::int64_t>(kSlots);
         ++tick) {
        if (head(static_cast<std::size_t>(tick % static_cast<std::int64_t>(kSlots))) != kNil) {
            const core::MonoTime at{core::Millis{tick * kTick.count()}};
            return std::max(core::Millis{0}, std::chrono::ceil<core::Millis>(at - now));
        }
    }
    return std::nullopt;
}

} // namespace net::detail
