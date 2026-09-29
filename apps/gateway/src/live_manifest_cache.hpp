#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/storage.hpp"
#include "core/util/time.hpp"
#include "net/offload_pool.hpp"

#include "playback.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gateway {

struct LiveCacheLimits {
    // A 10-segment window signs 11 URIs of about 600 bytes each, about 7.5 KB rewritten; the
    // longest (64 segments, a new init segment on each) about 85 KB. 4 MiB holds 512 typical
    // streams or 49 of the longest, 0.4% of the 1 GB box; past that the least recently
    // watched are read again when next asked for.
    std::size_t max_bytes = std::size_t{4} * 1024 * 1024;
    std::size_t max_entries = 512;
    // A viewer who opens a stream before its first playlist exists retries (hls.js backs off
    // from 1 s); one second bounds a flood of requests for a missing id to one store read a
    // second, and delays a stream's first viewers by at most that once it appears.
    core::Millis absent_for{1'000};
};

struct LiveCacheCounters {
    // Answered from a fresh copy, a playlist or a remembered absence.
    std::uint64_t hits = 0;
    // No fresh copy: the request started a fetch or joined the one in flight.
    std::uint64_t misses = 0;
    // Store reads started, one per miss that found no fetch in flight.
    std::uint64_t fetches = 0;
    // Misses that waited on a fetch another request had started.
    std::uint64_t joins = 0;
    // Copies dropped for the bounds while still fresh.
    std::uint64_t evictions = 0;
};

// What a waiter is handed. `body` is valid only during the call.
struct LiveAnswer {
    std::string_view body;
    bool ended = false;
};

class ILiveWaiter {
public:
    virtual ~ILiveWaiter() = default;
    // On the reactor thread, exactly once per get().
    virtual void
    on_live_playlist(const std::expected<LiveAnswer, PlaylistFailure>& answer) noexcept = 0;
};

// The rewritten live playlists of one gateway process, keyed by stream id. An LRU bounded by
// entries and bytes; each copy is fresh for as long as its playlist says (half its target
// duration, see build_live_playlist) counted from when its read began, and a miss for a stream
// already being fetched waits on that fetch instead of starting another, so any number of viewers
// of one stream cost one store read per freshness interval. Fetches run on the offload pool.
// Failures other than absence are handed to the requests waiting on that fetch and not kept: the
// next request tries again, and single-flight already limits a failing store to one read at a time
// per stream. Reactor thread only.
class LiveManifestCache {
public:
    struct Deps {
        net::OffloadPool& pool;
        // Thread-safe: read from the pool.
        core::ports::IObjectReader& reader;
        const core::ports::IClock& clock;
        // As build_playlist's.
        std::string_view local_read_url;
    };

    LiveManifestCache(Deps deps, LiveCacheLimits limits);
    // Flights still on the pool point here: the pool must be destroyed first.
    ~LiveManifestCache();
    LiveManifestCache(const LiveManifestCache&) = delete;
    LiveManifestCache& operator=(const LiveManifestCache&) = delete;

    // `waiter` hears back before this returns when a fresh copy is held, otherwise when the
    // fetch completes. It must stay alive until then.
    void get(std::string_view stream, ILiveWaiter& waiter) noexcept;
    // Frees flights whose waiters have all been answered. Call after each loop turn.
    void reap() noexcept;

    [[nodiscard]] const LiveCacheCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t entries() const noexcept { return lru_.size(); }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t flights() const noexcept { return flights_.size(); }

private:
    class Flight;

    struct Entry {
        std::string stream;
        // Empty for a remembered absence.
        std::string body;
        bool absent = false;
        bool ended = false;
        core::MonoTime expires;

        [[nodiscard]] std::size_t cost() const noexcept { return stream.size() + body.size(); }
    };

    void land(Flight& flight) noexcept;
    // Allocates, inside noexcept callers: running out of memory here ends the process, as it
    // does anywhere on the loop. The cache is 4 MiB of a 1 GB budget (ADR-0027); a failure to
    // allocate that much means the process is already lost, and unwinding half way through the
    // list and the index would leave them disagreeing.
    void remember(Entry entry, core::MonoTime now) noexcept;
    void erase(std::list<Entry>::iterator it) noexcept;

    Deps deps_;
    LiveCacheLimits limits_;
    LiveCacheCounters counters_;
    // Most recently used first. The index's keys view the entries' own stream strings.
    std::list<Entry> lru_;
    std::unordered_map<std::string_view, std::list<Entry>::iterator> index_;
    std::size_t bytes_ = 0;
    // Keyed by a view of the flight's own stream string.
    std::unordered_map<std::string_view, std::unique_ptr<Flight>> flights_;
    // Landed flights, destroyed by reap() rather than inside the pool's completion call.
    std::vector<std::unique_ptr<Flight>> landed_;
};

} // namespace gateway
