#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "../conformance/storage_harness.hpp"
#include "live_manifest_cache.hpp"
#include "support/fake_clock.hpp"
#include "support/reactor_harness.hpp"

#include <atomic>
#include <format>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

using core::ports::StorageError;
using gateway::LiveAnswer;
using gateway::LiveCacheLimits;
using gateway::LiveManifestCache;
using gateway::PlaylistFailure;
using ulw::test::pump_pending;
using ulw::test::pump_until;

// The store as the pool sees it: playlists by key, counted reads, and a failure to inject.
class ScriptedReader final : public core::ports::IObjectReader {
public:
    void put(const std::string& key, std::string text) {
        const std::scoped_lock lock(mutex_);
        objects_[key] = std::move(text);
    }
    void fail_with(std::optional<StorageError> error) {
        const std::scoped_lock lock(mutex_);
        failure_ = error;
    }
    [[nodiscard]] int reads() const { return reads_; }

    [[nodiscard]] std::expected<core::ports::ReadGrant, StorageError>
    grant_read(const core::StorageKey& key, core::Seconds ttl) override {
        return core::ports::ReadGrant{
            .kind = core::ports::ReadGrant::Kind::RedirectUrl,
            .value = std::format("https://store.test/{}?ttl={}", key.str(), ttl.count())};
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, StorageError>
    fetch_small(const core::StorageKey& key, std::size_t max) override {
        ++reads_;
        const std::scoped_lock lock(mutex_);
        if (failure_) {
            return std::unexpected(*failure_);
        }
        const auto it = objects_.find(key.str());
        if (it == objects_.end()) {
            return std::unexpected(StorageError::NotFound);
        }
        if (it->second.size() > max) {
            return std::unexpected(StorageError::Permanent);
        }
        const auto bytes = std::as_bytes(std::span(it->second));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::string> objects_;
    std::optional<StorageError> failure_;
    std::atomic<int> reads_{0};
};

struct Waiter final : gateway::ILiveWaiter {
    std::vector<std::expected<std::string, PlaylistFailure>> answers;
    std::vector<bool> ended;
    void
    on_live_playlist(const std::expected<LiveAnswer, PlaylistFailure>& answer) noexcept override {
        if (answer) {
            answers.emplace_back(std::string(answer->body));
            ended.push_back(answer->ended);
        } else {
            answers.emplace_back(std::unexpected(answer.error()));
            ended.push_back(false);
        }
    }
};

// What live_packager writes for a window of two segments.
std::string playlist(std::uint64_t sequence, int target = 2, bool ended = false) {
    std::string out = std::format("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:{}\n"
                                  "#EXT-X-MEDIA-SEQUENCE:{}\n#EXT-X-INDEPENDENT-SEGMENTS\n"
                                  "#EXT-X-MAP:URI=\"init_0.mp4\"\n",
                                  target, sequence);
    for (std::uint64_t n = sequence; n < sequence + 2; ++n) {
        out += std::format("#EXT-X-PROGRAM-DATE-TIME:2026-09-29T12:00:{:02}.000Z\n"
                           "#EXTINF:{}.000,\nseg_0_{}.m4s\n",
                           n % 60, target, n);
    }
    if (ended) {
        out += "#EXT-X-ENDLIST\n";
    }
    return out;
}

std::string key(std::string_view stream) {
    return "live/" + std::string(stream) + "/index.m3u8";
}

class LiveManifestCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor(ulw::test::reactor_kind_from_env(), system_clock_, 1024);
        ASSERT_TRUE(r);
        reactor_ = std::move(*r);
        auto p = net::OffloadPool::create(*reactor_, 4);
        ASSERT_TRUE(p);
        pool_ = std::move(*p);
    }
    void TearDown() override {
        pool_.reset();
        cache_.reset();
        reactor_.reset();
    }

    LiveManifestCache& cache(LiveCacheLimits limits = {}) {
        cache_ = std::make_unique<LiveManifestCache>(
            LiveManifestCache::Deps{
                .pool = *pool_, .reader = reader_, .clock = clock_, .local_read_url = {}},
            limits);
        return *cache_;
    }

    // Asks once and waits for the answer, however it comes.
    std::expected<std::string, PlaylistFailure> fetch(std::string_view stream) {
        Waiter w;
        cache_->get(stream, w);
        EXPECT_TRUE(pump_until(*reactor_, [&] { return !w.answers.empty(); }));
        cache_->reap();
        return w.answers.empty() ? std::unexpected(PlaylistFailure::Broken) : w.answers.front();
    }

    // Asks once and reports whether the answer came without the store: before get() returned.
    bool answered_at_once(std::string_view stream) {
        Waiter w;
        cache_->get(stream, w);
        const bool at_once = !w.answers.empty();
        EXPECT_TRUE(pump_until(*reactor_, [&] { return !w.answers.empty(); }));
        cache_->reap();
        return at_once;
    }

    os::SystemClock system_clock_;
    ulw::test::FakeClock clock_;
    ScriptedReader reader_;
    std::unique_ptr<net::IReactor> reactor_;
    std::unique_ptr<net::OffloadPool> pool_;
    std::unique_ptr<LiveManifestCache> cache_;
};

TEST_F(LiveManifestCacheTest, ConcurrentMissesForOneStreamCostOneStoreRead) {
    reader_.put(key("show"), playlist(7));
    LiveManifestCache& c = cache();
    constexpr int kViewers = 50;
    std::vector<Waiter> waiters(kViewers);
    // No completion runs until the loop is pumped, so all fifty ask while the first read is
    // still in flight.
    for (Waiter& w : waiters) {
        c.get("show", w);
    }
    EXPECT_EQ(c.flights(), 1U);
    ASSERT_TRUE(pump_until(*reactor_, [&] { return !waiters.back().answers.empty(); }));
    EXPECT_EQ(reader_.reads(), 1);
    EXPECT_EQ(c.counters().fetches, 1U);
    EXPECT_EQ(c.counters().joins, std::uint64_t{kViewers - 1});
    EXPECT_EQ(c.counters().misses, std::uint64_t{kViewers});
    for (const Waiter& w : waiters) {
        ASSERT_EQ(w.answers.size(), 1U);
        ASSERT_TRUE(w.answers.front());
        EXPECT_EQ(*w.answers.front(), *waiters.front().answers.front());
    }
    EXPECT_NE(waiters.front().answers.front()->find("#EXT-X-MEDIA-SEQUENCE:7\n"),
              std::string::npos);
    EXPECT_EQ(c.flights(), 0U);
}

TEST_F(LiveManifestCacheTest, EveryUriIsSignedForAnHourAgainstTheStreamsPrefix) {
    reader_.put(key("show"), playlist(3));
    cache();
    const auto body = fetch("show");
    ASSERT_TRUE(body);
    EXPECT_NE(body->find("#EXT-X-MAP:URI=\"https://store.test/live/show/init_0.mp4?ttl=3600\"\n"),
              std::string::npos)
        << *body;
    EXPECT_NE(body->find("\nhttps://store.test/live/show/seg_0_3.m4s?ttl=3600\n"),
              std::string::npos);
    EXPECT_NE(body->find("\nhttps://store.test/live/show/seg_0_4.m4s?ttl=3600\n"),
              std::string::npos);
    EXPECT_EQ(body->find("\nseg_"), std::string::npos);
}

TEST_F(LiveManifestCacheTest, ACopyIsFreshForHalfTheTargetDuration) {
    reader_.put(key("show"), playlist(0, 2));
    const LiveManifestCache& c = cache();
    ASSERT_TRUE(fetch("show"));
    reader_.put(key("show"), playlist(1, 2));
    clock_.advance(core::Millis{999});
    EXPECT_TRUE(answered_at_once("show"));
    EXPECT_EQ(reader_.reads(), 1);
    EXPECT_EQ(c.counters().hits, 1U);

    clock_.advance(core::Millis{1});
    const auto newer = fetch("show");
    ASSERT_TRUE(newer);
    EXPECT_EQ(reader_.reads(), 2);
    EXPECT_NE(newer->find("#EXT-X-MEDIA-SEQUENCE:1\n"), std::string::npos);
}

TEST_F(LiveManifestCacheTest, ALongerTargetDurationKeepsTheCopyLonger) {
    reader_.put(key("show"), playlist(0, 6));
    cache();
    ASSERT_TRUE(fetch("show"));
    clock_.advance(core::Millis{2'999});
    EXPECT_TRUE(answered_at_once("show"));
    clock_.advance(core::Millis{1});
    EXPECT_FALSE(answered_at_once("show"));
    EXPECT_EQ(reader_.reads(), 2);
}

TEST_F(LiveManifestCacheTest, AnEndedPlaylistPassesThroughAndIsKeptForAMinute) {
    reader_.put(key("show"), playlist(9, 2, true));
    cache();
    Waiter w;
    cache_->get("show", w);
    ASSERT_TRUE(pump_until(*reactor_, [&] { return !w.answers.empty(); }));
    ASSERT_TRUE(w.answers.front());
    EXPECT_TRUE(w.answers.front()->ends_with("#EXT-X-ENDLIST\n"));
    EXPECT_TRUE(w.ended.front());
    clock_.advance(core::Millis{59'999});
    EXPECT_TRUE(answered_at_once("show"));
    clock_.advance(core::Millis{1});
    EXPECT_FALSE(answered_at_once("show"));
}

TEST_F(LiveManifestCacheTest, AMissingStreamIsRememberedForASecondOnly) {
    LiveManifestCache& c = cache();
    EXPECT_EQ(fetch("soon"), std::unexpected(PlaylistFailure::Absent));
    reader_.put(key("soon"), playlist(0));
    clock_.advance(core::Millis{999});
    Waiter w;
    c.get("soon", w);
    ASSERT_EQ(w.answers.size(), 1U);
    EXPECT_EQ(w.answers.front(), std::unexpected(PlaylistFailure::Absent));
    EXPECT_EQ(reader_.reads(), 1);

    clock_.advance(core::Millis{1});
    EXPECT_TRUE(fetch("soon"));
    EXPECT_EQ(reader_.reads(), 2);
}

TEST_F(LiveManifestCacheTest, AStoreFailureReachesEveryWaiterAndIsNotKept) {
    reader_.fail_with(StorageError::Transient);
    LiveManifestCache& c = cache();
    std::vector<Waiter> waiters(3);
    for (Waiter& w : waiters) {
        c.get("show", w);
    }
    ASSERT_TRUE(pump_until(*reactor_, [&] { return !waiters.back().answers.empty(); }));
    for (const Waiter& w : waiters) {
        EXPECT_EQ(w.answers.front(), std::unexpected(PlaylistFailure::Unavailable));
    }
    EXPECT_EQ(reader_.reads(), 1);
    c.reap();

    // No time has passed: only a copy that was kept could answer without the store.
    reader_.fail_with(std::nullopt);
    reader_.put(key("show"), playlist(0));
    EXPECT_FALSE(answered_at_once("show"));
    EXPECT_EQ(reader_.reads(), 2);
    EXPECT_EQ(c.entries(), 1U);
}

TEST_F(LiveManifestCacheTest, ATargetDurationNoPackagerWritesIsRejectedAndNotKept) {
    reader_.put(key("odd"), playlist(0, 11));
    const LiveManifestCache& c = cache();
    EXPECT_EQ(fetch("odd"), std::unexpected(PlaylistFailure::Rejected));
    EXPECT_EQ(c.entries(), 0U);
}

TEST_F(LiveManifestCacheTest, TheLeastRecentlyWatchedStreamGoesPastTheEntryBound) {
    for (const char* s : {"a", "b", "c"}) {
        reader_.put(key(s), playlist(0));
    }
    const LiveManifestCache& c = cache({.max_entries = 2});
    ASSERT_TRUE(fetch("a"));
    ASSERT_TRUE(fetch("b"));
    EXPECT_TRUE(answered_at_once("a"));
    ASSERT_TRUE(fetch("c"));
    EXPECT_EQ(c.entries(), 2U);
    EXPECT_EQ(c.counters().evictions, 1U);
    EXPECT_TRUE(answered_at_once("a"));
    EXPECT_TRUE(answered_at_once("c"));
    EXPECT_FALSE(answered_at_once("b"));
}

TEST_F(LiveManifestCacheTest, TheByteBoundEvictsAndCountsWhatIsHeld) {
    for (const char* s : {"a", "b", "c"}) {
        reader_.put(key(s), playlist(0));
    }
    const LiveManifestCache& probe = cache();
    ASSERT_TRUE(fetch("a"));
    // One copy's cost: the rewritten body and its stream id.
    const std::size_t one = probe.bytes();
    ASSERT_GT(one, 0U);

    const LiveManifestCache& c = cache({.max_bytes = (2 * one) + 1});
    ASSERT_TRUE(fetch("a"));
    ASSERT_TRUE(fetch("b"));
    EXPECT_EQ(c.bytes(), 2 * one);
    ASSERT_TRUE(fetch("c"));
    EXPECT_EQ(c.entries(), 2U);
    EXPECT_LE(c.bytes(), (2 * one) + 1);
    EXPECT_FALSE(answered_at_once("a"));
}

TEST_F(LiveManifestCacheTest, ACopyLargerThanTheWholeBudgetIsServedButNotKept) {
    reader_.put(key("big"), playlist(0));
    const LiveManifestCache& c = cache({.max_bytes = 64});
    EXPECT_TRUE(fetch("big"));
    EXPECT_EQ(c.entries(), 0U);
    EXPECT_EQ(c.bytes(), 0U);
    EXPECT_FALSE(answered_at_once("big"));
}

// A waiter that asks again while it is being answered (the next pipelined request of a
// connection) must not join the fetch that is answering it, which would never answer again.
TEST_F(LiveManifestCacheTest, AskingAgainFromInsideAnAnswerGetsTheKeptCopy) {
    reader_.put(key("show"), playlist(0));
    LiveManifestCache& c = cache();
    struct Again final : gateway::ILiveWaiter {
        LiveManifestCache* cache = nullptr;
        Waiter second;
        int calls = 0;
        void on_live_playlist(
            const std::expected<LiveAnswer, PlaylistFailure>& /*a*/) noexcept override {
            ++calls;
            cache->get("show", second);
        }
    } again;
    again.cache = &c;
    c.get("show", again);
    ASSERT_TRUE(pump_until(*reactor_, [&] { return again.calls == 1; }));
    ASSERT_EQ(again.second.answers.size(), 1U);
    EXPECT_TRUE(again.second.answers.front());
    EXPECT_EQ(reader_.reads(), 1);
    pump_pending(*reactor_);
    EXPECT_EQ(again.calls, 1);
}

} // namespace
