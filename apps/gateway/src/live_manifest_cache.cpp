#include "live_manifest_cache.hpp"

#include <optional>
#include <utility>

namespace gateway {

class LiveManifestCache::Flight final : public net::IOffloadJob {
public:
    Flight(LiveManifestCache& cache, std::string stream, core::MonoTime started)
        : cache_(cache), stream_(std::move(stream)), started_(started) {}

    // On the pool. Only result_ is written, and nothing reads it before complete().
    void run() noexcept override {
        result_ = build_live_playlist(cache_.deps_.reader, stream_, cache_.deps_.local_read_url);
    }
    void complete() noexcept override { cache_.land(*this); }

    [[nodiscard]] const std::string& stream() const noexcept { return stream_; }
    [[nodiscard]] core::MonoTime started() const noexcept { return started_; }
    [[nodiscard]] const std::optional<std::expected<LivePlaylist, PlaylistFailure>>&
    result() const noexcept {
        return result_;
    }
    std::vector<ILiveWaiter*>& waiters() noexcept { return waiters_; }

private:
    LiveManifestCache& cache_;
    std::string stream_;
    core::MonoTime started_;
    std::vector<ILiveWaiter*> waiters_;
    std::optional<std::expected<LivePlaylist, PlaylistFailure>> result_;
};

LiveManifestCache::LiveManifestCache(Deps deps, LiveCacheLimits limits)
    : deps_(deps), limits_(limits) {}

LiveManifestCache::~LiveManifestCache() = default;

void LiveManifestCache::get(std::string_view stream, ILiveWaiter& waiter) noexcept {
    if (const auto found = index_.find(stream); found != index_.end()) {
        const auto it = found->second;
        if (deps_.clock.now() < it->expires) {
            ++counters_.hits;
            lru_.splice(lru_.begin(), lru_, it);
            if (it->absent) {
                waiter.on_live_playlist(std::unexpected(PlaylistFailure::Absent));
            } else {
                waiter.on_live_playlist(LiveAnswer{.body = it->body, .ended = it->ended});
            }
            return;
        }
        erase(it);
    }
    ++counters_.misses;
    if (const auto flying = flights_.find(stream); flying != flights_.end()) {
        ++counters_.joins;
        flying->second->waiters().push_back(&waiter);
        return;
    }
    ++counters_.fetches;
    auto flight = std::make_unique<Flight>(*this, std::string(stream), deps_.clock.now());
    flight->waiters().push_back(&waiter);
    Flight& job = *flight;
    flights_.emplace(job.stream(), std::move(flight));
    deps_.pool.submit(job);
}

void LiveManifestCache::land(Flight& flight) noexcept {
    const auto node = flights_.extract(flight.stream());
    if (node.empty()) {
        return;
    }
    // Out of flights_ before any waiter runs: a waiter that asks for the stream again (a
    // pipelined request) must see the stored copy or start a fetch of its own, not join this
    // one after its waiters have been answered.
    landed_.push_back(std::move(node.mapped()));
    const auto& result = flight.result();
    const auto answer = [&]() -> std::expected<LiveAnswer, PlaylistFailure> {
        if (!result) {
            return std::unexpected(PlaylistFailure::Broken);
        }
        if (!*result) {
            return std::unexpected(result->error());
        }
        return LiveAnswer{.body = (*result)->body, .ended = (*result)->ended};
    }();
    // Freshness runs from the moment the read began, not from its landing: the copy is at
    // least that old, so a slow store read shortens how long it is served rather than
    // stretching it past T/2.
    const core::MonoTime now = deps_.clock.now();
    if (result && *result) {
        remember({.stream = flight.stream(),
                  .body = (*result)->body,
                  .ended = (*result)->ended,
                  .expires = flight.started() + (*result)->fresh_for},
                 now);
    } else if (result && result->error() == PlaylistFailure::Absent) {
        remember({.stream = flight.stream(),
                  .body = {},
                  .absent = true,
                  .expires = flight.started() + limits_.absent_for},
                 now);
    }
    for (ILiveWaiter* waiter : flight.waiters()) {
        waiter->on_live_playlist(answer);
    }
    flight.waiters().clear();
}

void LiveManifestCache::remember(Entry entry, core::MonoTime now) noexcept {
    if (const auto stale = index_.find(entry.stream); stale != index_.end()) {
        erase(stale->second);
    }
    // One copy bigger than the whole budget is served but never kept, rather than emptying
    // the cache for it; nor is one whose read took longer than it stays fresh.
    if (entry.cost() > limits_.max_bytes || limits_.max_entries == 0 || entry.expires <= now) {
        return;
    }
    while (!lru_.empty() &&
           (lru_.size() >= limits_.max_entries || bytes_ + entry.cost() > limits_.max_bytes)) {
        const auto oldest = std::prev(lru_.end());
        // Only a copy that could still have been served counts: dropping a stale one costs
        // no store read, and counting it would make the metric rise with every new stream
        // once the table is full of ids nobody asks for any more.
        if (now < oldest->expires) {
            ++counters_.evictions;
        }
        erase(oldest);
    }
    bytes_ += entry.cost();
    lru_.push_front(std::move(entry));
    index_.emplace(lru_.front().stream, lru_.begin());
}

void LiveManifestCache::erase(std::list<Entry>::iterator it) noexcept {
    bytes_ -= it->cost();
    index_.erase(it->stream);
    lru_.erase(it);
}

void LiveManifestCache::reap() noexcept {
    landed_.clear();
}

} // namespace gateway
