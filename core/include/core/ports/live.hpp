#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core::ports {

// Where a live stream the stream service started stands (ADR-0092). It only moves forward:
// Starting (the owner has a publisher ticket, nothing is relayed yet), Live (the stream's
// packager runs and the media server relays the publisher to it), Ended.
enum class LiveState : std::uint8_t {
    Starting,
    Live,
    Ended,
};

// Why a stream ended.
enum class LiveEnd : std::uint8_t {
    // Its owner asked.
    Owner,
    // Its packager ended it: the publisher went (a WHIP DELETE, or dropped by the media
    // server), the maximum duration, or a stream that broke (ADR-0047).
    Finished,
    // Its packager could not run, or exited for good without ending it.
    Failed,
    // Nobody went live within the start window, or it outlived the longest stream.
    Timeout,
    // LiveKit said its publisher left the room, or the room finished, and the publisher was
    // still gone after a grace for reconnects (ADR-0093).
    PublisherLeft,
};

[[nodiscard]] std::string_view to_string(LiveState s) noexcept;
[[nodiscard]] std::string_view to_string(LiveEnd e) noexcept;

struct LiveStream {
    LiveStreamId id;
    // The broadcaster: the only user who may publish, go live, end it, or see its recording.
    UserId owner;
    LiveState state = LiveState::Starting;
    // What the packager admits the relay with (ADR-0046). A secret: never logged, never sent
    // to a client.
    std::string passphrase;
    WallTime created_at;
    std::optional<WallTime> live_at;
    std::optional<WallTime> ended_at;
    std::optional<LiveEnd> ended_by;
    // The video the packager queued the ended stream's recording as (ADR-0055), once it has.
    std::optional<VideoId> recording;
};

enum class LiveStoreError : std::uint8_t {
    NotFound,
    // As many streams are unfinished as the platform takes at once.
    Full,
    // The owner has created as many streams in the last hour as one may.
    TooMany,
    // The database is unreachable or refused the statement; the request may be retried.
    Unavailable,
    // A stored row violates an invariant.
    Corrupt,
};

[[nodiscard]] std::string_view to_string(LiveStoreError e) noexcept;

template <class T> using LiveResult = std::expected<T, LiveStoreError>;
// Called exactly once, on the reactor thread, never from inside the call that was given it.
template <class T> using LiveCallback = std::move_only_function<void(LiveResult<T>) noexcept>;

struct NewLiveStream {
    LiveStreamId id;
    UserId owner;
    std::string passphrase;
    WallTime at;
};

// What a create is held to: streams unfinished on the platform at once, and streams one owner
// may create in an hour, whatever became of them.
struct LiveLimits {
    std::int64_t max_unfinished = 2;
    std::int64_t per_owner_per_hour = 6;
};

struct CreatedLiveStream {
    LiveStream stream;
    // False: the owner already had an unfinished stream, which is this one, and nothing new was
    // stored.
    bool created = false;
};

struct EndedLiveStream {
    LiveStream stream;
    // False: it had ended already, and the first end stands.
    bool ended = false;
};

// The streams' rows. A user has at most one unfinished stream; the store keeps that true
// across every gateway process, whatever the order requests arrive in.
class ILiveStreamStore {
public:
    virtual ~ILiveStreamStore() = default;

    // Stores the stream, Starting, and opens its live chat, unless its owner already has an
    // unfinished one (answered with that one), `max_unfinished` streams are unfinished already
    // (Full), or the owner created `per_owner_per_hour` in the hour before (TooMany). Creates
    // are serialised, so concurrent ones never pass a cap together.
    virtual void create(NewLiveStream stream, LiveLimits limits,
                        LiveCallback<CreatedLiveStream> done) = 0;
    virtual void find(const LiveStreamId& id, LiveCallback<LiveStream> done) = 0;
    // Starting or Live becomes Live, live since `at` unless it already was; an ended stream is
    // answered as it is.
    virtual void mark_live(const LiveStreamId& id, WallTime at, LiveCallback<LiveStream> done) = 0;
    // Ends the stream at `at` for `reason`, unless it has ended already, and closes its live
    // chat to new joins with it; answers the stream as it then stands, so a repeat sees the
    // first end.
    virtual void end(const LiveStreamId& id, LiveEnd reason, WallTime at,
                     LiveCallback<EndedLiveStream> done) = 0;
    // Up to `limit` unfinished streams, oldest first.
    virtual void unfinished(std::size_t limit, LiveCallback<std::vector<LiveStream>> done) = 0;
};

// What one stream's packager is given when it starts (ADR-0083): the stream, the owner its
// recording belongs to, and the secret its listener admits the relay with.
struct PackagerSpec {
    LiveStreamId stream;
    UserId owner;
    std::string passphrase;
};

enum class PackagerState : std::uint8_t {
    // Nothing runs for the stream, and nothing did that is still known of.
    Absent,
    // Asked for, not listening yet.
    Starting,
    // Its listener is up: the relay may call it.
    Ready,
    // It ran to its end: the stream ended, and its recording is queued or settled.
    Finished,
    // It failed for good.
    Failed,
};

[[nodiscard]] std::string_view to_string(PackagerState s) noexcept;

enum class PackagerError : std::uint8_t {
    // The runtime that starts packagers did not answer, or is busy; a retry may succeed.
    Unavailable,
    // It refused the request as made (credentials, permissions, a bad template); a retry
    // cannot succeed until the configuration changes.
    Refused,
};

[[nodiscard]] std::string_view to_string(PackagerError e) noexcept;

using PackagerDone = std::move_only_function<void(std::expected<void, PackagerError>) noexcept>;
using PackagerStateDone =
    std::move_only_function<void(std::expected<PackagerState, PackagerError>) noexcept>;

// Starts and watches one packager per stream: a process beside the gateway, or a Kubernetes Job
// (ADR-0083, ADR-0092). A packager ends its stream by itself once its publisher goes, or once
// none has come for a while after it starts, so there is no call to stop one. Every member runs
// on the reactor thread, and every callback there later, never from inside the call.
class IPackagers {
public:
    virtual ~IPackagers() = default;
    // Idempotent: a packager already started for the stream, running or not, is left as it is.
    virtual void start(const PackagerSpec& spec, PackagerDone done) = 0;
    virtual void state(const LiveStreamId& stream, PackagerStateDone done) = 0;
};

} // namespace core::ports
