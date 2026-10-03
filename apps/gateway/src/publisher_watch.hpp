#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"
#include "net/reactor.hpp"

#include "live_streams.hpp"
#include "livekit_webhook.hpp"
#include "ops/log.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gateway {

// What a verified webhook is handed to.
class IWebhookSink {
public:
    virtual ~IWebhookSink() = default;
    virtual void on_event(const WebhookEvent& event) noexcept = 0;

protected:
    IWebhookSink() = default;
    IWebhookSink(const IWebhookSink&) = default;
    IWebhookSink(IWebhookSink&&) = default;
    IWebhookSink& operator=(const IWebhookSink&) = default;
    IWebhookSink& operator=(IWebhookSink&&) = default;
};

struct WatchSettings {
    // How long a publisher may be gone before its stream ends: LiveKit reports a full
    // reconnect as the old session leaving and a new one joining, seconds apart
    // (ULW_LIVE_PUBLISHER_GRACE_SECONDS).
    core::Millis grace{10'000};
    // A start or an end check a dependency could not answer is tried again this long after,
    // this many times; the stream service's own sweep covers whatever is left after that.
    core::Millis retry{2'000};
    std::uint32_t attempts = 5;
    // Streams followed at once. Each is a few hundred bytes; events for further streams are
    // still acted on, one at a time, but not followed.
    std::size_t max_streams = 1024;
};

struct WatchCounters {
    std::uint64_t events = 0;
    // Not about any stream's publisher: a call's room, LiveKit's recorder, another event.
    std::uint64_t ignored = 0;
    // About a session already seen leaving: delivered late or twice.
    std::uint64_t stale = 0;
    std::uint64_t starts = 0;
    std::uint64_t start_failures = 0;
    // The publisher was gone and the grace began; and how often it came back within it.
    std::uint64_t departures = 0;
    std::uint64_t returns = 0;
    // The grace ran out and the media server still had the publisher.
    std::uint64_t kept = 0;
    std::uint64_t ended = 0;
    std::uint64_t check_failures = 0;
    // A stream that could not be followed, all max_streams being followed already.
    std::uint64_t untracked = 0;
};

// Turns LiveKit's word about a stream's publisher into the stream service's operations
// (ADR-0093): the publisher joined or published a track, so the stream goes live through the
// same idempotent start the owner's POST .../start takes; the publisher left, or its room
// finished, so the stream ends once the publisher has stayed gone for the grace and the media
// server confirms it. Events may come late, twice, out of order, or for rooms that are no
// stream's: a session seen leaving is never brought back by a late join, and every decision
// that ends a stream asks the media server and the stream's row first.
//
// Reactor thread only. A replica sees only the events LiveKit sent it, so nothing here is the
// only copy of anything: the rows and the media server are.
class PublisherWatch final : public IWebhookSink {
public:
    PublisherWatch(net::IReactor& reactor, LiveStreams& live, ops::Logger& log,
                   WatchSettings settings);
    ~PublisherWatch() override;
    PublisherWatch(const PublisherWatch&) = delete;
    PublisherWatch& operator=(const PublisherWatch&) = delete;

    void on_event(const WebhookEvent& event) noexcept override;

    [[nodiscard]] const WatchCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] const WatchSettings& settings() const noexcept { return settings_; }
    // Streams followed now.
    [[nodiscard]] std::size_t followed() const noexcept { return streams_.size(); }
    // The stream's grace is running.
    [[nodiscard]] bool in_grace(const core::LiveStreamId& id) const noexcept;

private:
    struct Followed;

    void joined(Followed& stream, const core::UserId& owner, const std::string& session) noexcept;
    void left(Followed& stream, const std::string& session) noexcept;
    void room_gone(Followed& stream) noexcept;
    void start(Followed& stream) noexcept;
    void check(Followed& stream) noexcept;
    void on_timer(Followed& stream) noexcept;
    void forget(const std::string& key) noexcept;
    [[nodiscard]] Followed* follow(const core::LiveStreamId& id) noexcept;
    [[nodiscard]] Followed* find(const std::string& key) noexcept;

    net::IReactor& reactor_;
    LiveStreams& live_;
    ops::Logger& log_;
    WatchSettings settings_;
    WatchCounters counters_;
    std::map<std::string, std::unique_ptr<Followed>, std::less<>> streams_;
    // Orders the streams by when they were last heard of, for making room.
    std::uint64_t stamp_ = 0;
    // Callbacks from the stream service check this before touching anything here.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace gateway
