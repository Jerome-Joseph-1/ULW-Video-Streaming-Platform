#pragma once

#include "core/models/ids.hpp"
#include "core/ports/clock.hpp"
#include "core/ports/live.hpp"
#include "core/ports/media.hpp"
#include "core/ports/random.hpp"
#include "net/reactor.hpp"

#include "ops/log.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace gateway {

// The stream service (ADR-0091): starts a user's live stream, hands its owner publisher tickets,
// starts the stream's packager and the relay to it when the owner goes live, ends it when the
// owner asks, and notices when it has ended by itself. The rows live in Postgres, so any gateway
// process can serve any request for any stream; nothing here is the only copy of anything.
struct LiveSettings {
    // The packager's segment length, which the relay's keyframe interval must equal
    // (ADR-0046); the Job template's ULW_LIVE_SEGMENT_SECONDS.
    core::Seconds segment{2};
    // Unfinished streams the platform takes at once: what egress can relay (RUNBOOK step 9).
    std::uint32_t max_streams = 2;
    // How long going live waits for the stream's packager to listen: a Job's pod is scheduled
    // and its image pulled first.
    core::Millis ready_wait{30'000};
    core::Millis ready_poll{500};
    // A stream nobody took live in this long is ended. Its tickets lasted a minute each, so the
    // window is the owner's time to set up an encoder, not a credential's lifetime.
    core::Seconds start_window{600};
    // The longest stream (ULW_LIVE_MAX_HOURS, 12 h) and an hour for its recording: past this
    // the stream is ended whatever its packager says.
    core::Seconds max_age{std::chrono::hours(13)};
    // How often unfinished streams are looked at, and how many at a time.
    core::Millis sweep_interval{10'000};
    std::size_t sweep_batch = 64;
};

struct LiveDeps {
    net::IReactor& reactor;
    core::ports::ILiveStreamStore& store;
    core::ports::ISfu& sfu;
    core::ports::IPackagers& packagers;
    const core::ports::IClock& clock;
    core::ports::IRandom& random;
    ops::Logger& log;
};

enum class LiveFailure : std::uint8_t {
    // No such stream, or not the caller's to act on: the two read alike.
    NotFound,
    // The stream has ended; nothing more can be done with it.
    Ended,
    // The platform runs as many streams as it takes.
    Full,
    // A dependency did not answer or is busy; a retry may succeed.
    Unavailable,
    // A dependency refused the request as made, or a stored row is broken.
    Internal,
};

template <class T>
using LiveDone = std::move_only_function<void(std::expected<T, LiveFailure>) noexcept>;

struct StartedStream {
    core::ports::LiveStream stream;
    core::ports::MediaTicket ticket;
    // False: the owner's unfinished stream, answered again.
    bool created = false;
};

// What publisher_left found.
enum class Departure : std::uint8_t {
    // The publisher was gone at the media server, and the stream ended for it now.
    Ended,
    // The media server still has the publisher connected: it came back, or never left.
    Present,
    // Not live: still starting, which only its start window ends, or ended already.
    NotLive,
};

struct LiveCounters {
    std::uint64_t created = 0;
    std::uint64_t tickets = 0;
    std::uint64_t went_live = 0;
    // By core::ports::LiveEnd.
    std::array<std::uint64_t, 5> ended{};
    std::uint64_t store_failures = 0;
    std::uint64_t media_failures = 0;
    std::uint64_t packager_failures = 0;
    std::uint64_t sweeps = 0;
};

// Reactor thread only. Every callback runs later, never inside the call that was given it, and
// exactly once while this lives; destroying it drops the callbacks still pending.
class LiveStreams final : public net::ITimerHandler {
public:
    LiveStreams(LiveDeps deps, LiveSettings settings);
    ~LiveStreams() override;
    LiveStreams(const LiveStreams&) = delete;
    LiveStreams& operator=(const LiveStreams&) = delete;

    // A new stream for `owner`, with its first publisher ticket, or the owner's unfinished one
    // with a fresh ticket.
    void create(const core::UserId& owner, LiveDone<StartedStream> done);
    // A fresh publisher ticket, for the stream's owner, while it has not ended.
    void ticket(const core::LiveStreamId& id, const core::UserId& owner,
                LiveDone<core::ports::MediaTicket> done);
    // The owner's WHIP POST has been answered: starts the packager, waits for it to listen,
    // and relays the publisher to it. Idempotent.
    void go_live(const core::LiveStreamId& id, const core::UserId& owner,
                 LiveDone<core::ports::LiveStream> done);
    // Ends the stream for its owner. Idempotent: ending an ended stream closes its room again.
    void end(const core::LiveStreamId& id, const core::UserId& owner,
             LiveDone<core::ports::LiveStream> done);
    // LiveKit said the stream's publisher left, or its room finished, and the grace for a
    // reconnect has passed (ADR-0093): a live stream whose publisher the media server no longer
    // has ends for that reason. Asking the media server, not believing the event, keeps a
    // publisher that came back through another gateway replica on air.
    void publisher_left(const core::LiveStreamId& id, LiveDone<Departure> done);
    // The stream as any viewer may see it.
    void status(const core::LiveStreamId& id, LiveDone<core::ports::LiveStream> done);
    // A viewer found the stream's playlist ended: the packager has ended it.
    void playlist_ended(const core::LiveStreamId& id);

    // Looks at the unfinished streams every sweep_interval from now on.
    void start_sweeping() noexcept;
    // Looks now, and every sweep_interval after this sweep ends.
    void sweep_now() noexcept;
    void on_timeout() noexcept override;

    [[nodiscard]] const LiveCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] const LiveSettings& settings() const noexcept { return settings_; }
    // Operations still waiting on a dependency.
    [[nodiscard]] std::size_t pending() const noexcept { return pending_; }
    // A sweep is under way.
    [[nodiscard]] bool sweeping() const noexcept { return sweeping_; }

private:
    class Delay;
    using Stream = core::ports::LiveStream;

    void issue_ticket(const Stream& stream, LiveDone<core::ports::MediaTicket> done);
    // Finds the stream and checks that `owner` owns it.
    void find_owned(const core::LiveStreamId& id, const core::UserId& owner, LiveDone<Stream> done);
    void await_packager(Stream stream, core::MonoTime deadline, LiveDone<Stream> done);
    void relay(Stream stream, LiveDone<Stream> done);
    // Ends the stream, then closes its room.
    void finish(const core::LiveStreamId& id, core::ports::LiveEnd reason, LiveDone<Stream> done);
    void close_room(Stream stream, LiveDone<Stream> done);
    void after(core::Millis delay, std::move_only_function<void() noexcept> then);
    void sweep() noexcept;
    void sweep_next() noexcept;
    void sweep_one(const Stream& stream, std::move_only_function<void() noexcept> next);
    void sweep_done() noexcept;
    void reap_delays() noexcept;

    [[nodiscard]] LiveFailure store_failure(core::ports::LiveStoreError e) noexcept;
    [[nodiscard]] LiveFailure media_failure(core::ports::MediaError e) noexcept;
    [[nodiscard]] LiveFailure packager_failure(core::ports::PackagerError e) noexcept;

    LiveDeps deps_;
    LiveSettings settings_;
    LiveCounters counters_;
    std::vector<std::unique_ptr<Delay>> delays_;
    std::optional<net::TimerId> sweep_timer_;
    std::vector<Stream> sweep_list_;
    std::size_t sweep_index_ = 0;
    bool sweeping_ = false;
    std::size_t pending_ = 0;
};

[[nodiscard]] std::string_view to_string(LiveFailure f) noexcept;

} // namespace gateway
