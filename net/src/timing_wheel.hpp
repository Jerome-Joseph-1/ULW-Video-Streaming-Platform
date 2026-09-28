#pragma once

#include "net/reactor.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace net::detail {

// Hashed wheel: 512 slots x 100 ms covers 51.1 s per revolution, enough for every
// connection timeout except the 6 h request backstop, which re-inserts itself each time its
// slot comes round. Timers fire up to one tick late; nothing here needs better precision.
class TimingWheel {
public:
    static constexpr std::size_t kSlots = 512;
    static constexpr core::Millis kTick{100};

    explicit TimingWheel(core::MonoTime now) noexcept;

    TimerId arm(core::MonoTime now, core::Millis delay, ITimerHandler& handler);
    void cancel(TimerId id) noexcept;
    std::size_t tick_to(core::MonoTime now) noexcept;
    // How long until the next occupied slot fires, or nullopt when nothing is armed.
    [[nodiscard]] std::optional<core::Millis> next_expiry(core::MonoTime now) const noexcept;
    [[nodiscard]] std::size_t armed() const noexcept { return armed_; }

private:
    static constexpr std::uint32_t kNil = UINT32_MAX;

    struct Entry {
        core::MonoTime deadline;
        ITimerHandler* handler = nullptr;
        std::uint32_t gen = 1;
        std::uint32_t prev = kNil;
        std::uint32_t next = kNil;
        std::uint32_t slot = kNil;
    };

    [[nodiscard]] static std::int64_t tick_of(core::MonoTime t) noexcept;
    void insert(std::uint32_t index) noexcept;
    void unlink(std::uint32_t index) noexcept;
    void release(std::uint32_t index) noexcept;

    std::vector<Entry> entries_;
    std::vector<std::uint32_t> free_;
    std::array<std::uint32_t, kSlots> heads_{};
    std::int64_t last_tick_;
    std::size_t armed_ = 0;
};

} // namespace net::detail
