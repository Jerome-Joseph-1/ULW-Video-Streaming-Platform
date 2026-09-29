#include "health.hpp"

#include "ops/process.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace gateway {

namespace {

std::int64_t millis(core::MonoTime t) noexcept {
    return std::chrono::duration_cast<core::Millis>(t.time_since_epoch()).count();
}

} // namespace

void Health::record(bool database_up, bool store_up, core::MonoTime at) noexcept {
    database_up_.store(database_up, std::memory_order_relaxed);
    store_up_.store(store_up, std::memory_order_relaxed);
    probed_at_ms_.store(millis(at), std::memory_order_relaxed);
}

void Health::set_oldest_queued(std::optional<core::Seconds> age) noexcept {
    const auto seconds = age ? std::max<core::Seconds::rep>(age->count(), 0) : 0;
    oldest_queued_.store(static_cast<std::uint64_t>(seconds), std::memory_order_relaxed);
}

void Health::set_process(std::optional<std::uint64_t> fds,
                         std::optional<std::uint64_t> resident) noexcept {
    if (fds) {
        fds_.store(*fds, std::memory_order_relaxed);
    }
    if (resident) {
        resident_.store(*resident, std::memory_order_relaxed);
    }
}

Readiness Health::readiness(core::MonoTime now) const noexcept {
    const std::int64_t at = probed_at_ms_.load(std::memory_order_relaxed);
    if (at == 0) {
        return Readiness::Starting;
    }
    if (millis(now) - at > kProbeStale.count()) {
        return Readiness::Stale;
    }
    if (!database_up()) {
        return Readiness::DatabaseDown;
    }
    if (!store_up()) {
        return Readiness::StoreDown;
    }
    return Readiness::Ready;
}

HealthProbe::HealthProbe(Health& health, ProbeChecks checks, const core::ports::IClock& clock,
                         ops::Logger& log)
    : health_(health), checks_(std::move(checks)), clock_(clock), log_(log) {}

void HealthProbe::probe_once() {
    const auto database = checks_.database();
    const auto store = checks_.store();
    if (database) {
        health_.set_oldest_queued(*database);
    }
    health_.set_process(ops::open_descriptors(), ops::resident_bytes());
    if (checks_.store_paging_errors) {
        health_.set_store_paging_errors(checks_.store_paging_errors());
    }
    health_.record(database.has_value(), store.has_value(), clock_.now());

    // Changes only: a dependency that stays down is one line, not one every five seconds.
    const auto report = [&](std::string_view name, std::optional<bool>& was, bool up,
                            std::string_view why) {
        if (was == up) {
            return;
        }
        was = up;
        if (up) {
            log_.info("dependency up", {{"dependency", name}});
        } else {
            log_.warn("dependency down", {{"dependency", name}, {"error", why}});
        }
    };
    report("database", database_was_up_, database.has_value(),
           database ? std::string_view{} : std::string_view(database.error()));
    report("store", store_was_up_, store.has_value(),
           store ? std::string_view{} : std::string_view(store.error()));
}

void HealthProbe::start(core::Millis interval) {
    thread_ = std::jthread([this, interval](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            probe_once();
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, stop, interval, [] { return false; });
        }
    });
}

} // namespace gateway
