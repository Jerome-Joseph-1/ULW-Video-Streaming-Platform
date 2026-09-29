#pragma once

#include "core/ports/clock.hpp"

#include "ops/log.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace gateway {

enum class Readiness : std::uint8_t { Ready, Starting, DatabaseDown, StoreDown, Stale };

// A probe runs every 5 s: a dependency that fails is noticed within one interval, and the
// probe costs its database and store one cheap request each per interval per replica.
inline constexpr core::Millis kProbeInterval{5'000};
// A probe still unfinished this long after the last one is stuck, and so, as far as readiness
// is concerned, is what it waits on. A healthy probe takes at most the database's 5 s connect
// timeout plus its 2 s statement timeout, then the store's 6.3 s of retry backoff and the
// requests between them; 30 s is well past both and still short of an operator's patience.
inline constexpr core::Millis kProbeStale{30'000};
// A dependency counts as down after this many failed probes in a row. The probe's database
// session is replaced on the call after it breaks, so a restart or failover of the database
// costs one failed probe even when the new server answers the next; a store answering one
// request with a 503 costs one too. One failure turning every replica unready together would
// turn a blip into an outage; two in a row, at least 5 s apart, is a dependency that is down,
// and /readyz says so within 10 s.
inline constexpr std::uint32_t kMissesBeforeUnready = 2;

// What the probe thread last learned, for the loop to read. Every field stands alone, so
// relaxed atomics suffice; none of them publishes anything else.
class Health {
public:
    void record(bool database_up, bool store_up, core::MonoTime at) noexcept;
    // `age` of nullopt: nothing waits. Called only with an answer from the database.
    void set_oldest_queued(std::optional<core::Seconds> age) noexcept;
    // The database did not answer, so nobody knows how old the queue is.
    void forget_oldest_queued() noexcept { oldest_queued_.store(-1, std::memory_order_relaxed); }
    void set_process(std::optional<std::uint64_t> fds,
                     std::optional<std::uint64_t> resident) noexcept;
    void set_store_paging_errors(std::uint64_t n) noexcept {
        paging_errors_.store(n, std::memory_order_relaxed);
    }

    // A dependency that has never answered is down at once; one that has, only after
    // kMissesBeforeUnready failed probes in a row.
    [[nodiscard]] Readiness readiness(core::MonoTime now) const noexcept;
    // What the last probe found, for the dependency_up gauge.
    [[nodiscard]] bool database_up() const noexcept {
        return database_up_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool store_up() const noexcept {
        return store_up_.load(std::memory_order_relaxed);
    }
    // Zero when nothing is waiting; nullopt while the last probe could not ask.
    [[nodiscard]] std::optional<std::uint64_t> oldest_queued_seconds() const noexcept {
        const std::int64_t age = oldest_queued_.load(std::memory_order_relaxed);
        return age < 0 ? std::nullopt : std::optional(static_cast<std::uint64_t>(age));
    }
    [[nodiscard]] std::uint64_t open_fds() const noexcept {
        return fds_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t resident_bytes() const noexcept {
        return resident_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t store_paging_errors() const noexcept {
        return paging_errors_.load(std::memory_order_relaxed);
    }

private:
    // Failed probes in a row; kNeverUp until the first success.
    static constexpr std::uint32_t kNeverUp = 0xFFFF'FFFF;
    static void count(std::atomic<std::uint32_t>& misses, bool up) noexcept;

    std::atomic<bool> database_up_{false};
    std::atomic<bool> store_up_{false};
    std::atomic<std::uint32_t> database_misses_{kNeverUp};
    std::atomic<std::uint32_t> store_misses_{kNeverUp};
    // Steady-clock milliseconds; zero until the first probe finishes.
    std::atomic<std::int64_t> probed_at_ms_{0};
    // -1 until the database has answered, and again whenever it does not.
    std::atomic<std::int64_t> oldest_queued_{-1};
    std::atomic<std::uint64_t> fds_{0};
    std::atomic<std::uint64_t> resident_{0};
    std::atomic<std::uint64_t> paging_errors_{0};
};

struct ProbeChecks {
    // The age of the oldest job due to run, nullopt when none is; an error when the database
    // cannot be asked.
    std::function<std::expected<std::optional<core::Seconds>, std::string>()> database;
    std::function<std::expected<void, std::string>()> store;
    // Empty for a store that keeps no such count.
    std::function<std::uint64_t()> store_paging_errors;
};

// Asks the database and the store whether they answer, off the loop: both calls block, the
// store's for as long as its retries take. The loop only ever reads the answers.
class HealthProbe {
public:
    HealthProbe(Health& health, ProbeChecks checks, const core::ports::IClock& clock,
                ops::Logger& log);
    // Stops and joins the thread; a probe in progress finishes first.
    ~HealthProbe() = default;
    HealthProbe(const HealthProbe&) = delete;
    HealthProbe& operator=(const HealthProbe&) = delete;
    HealthProbe(HealthProbe&&) = delete;
    HealthProbe& operator=(HealthProbe&&) = delete;

    void probe_once();
    // Probes now and then every `interval` on a thread of its own, until stop().
    void start(core::Millis interval);
    // No probe starts after this; one in progress finishes, and destruction waits for it. Called
    // when the drain begins, so that a probe stuck on a dead dependency (5 s connect, 2 s
    // statement, 6.3 s of store retries) runs out during the drain rather than after it, and
    // the exit stays inside the manager's 45 s.
    void stop() noexcept { thread_.request_stop(); }

private:
    Health& health_;
    ProbeChecks checks_;
    const core::ports::IClock& clock_;
    ops::Logger& log_;
    // Only the probing thread reads or writes these.
    std::optional<bool> database_was_up_;
    std::optional<bool> store_was_up_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::jthread thread_;
};

} // namespace gateway
