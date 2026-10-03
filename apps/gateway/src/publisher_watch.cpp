#include "publisher_watch.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace gateway {

namespace {

// Sessions seen leaving, remembered per stream so that a join delivered after its own leave
// changes nothing. A publisher that reconnects more often than this within one stream's life
// has bigger problems than a late event.
constexpr std::size_t kGoneSessions = 16;

} // namespace

// One stream's publisher as this replica has heard of it. Its state is the watch's to read and
// write, so all of it is open to it.
struct PublisherWatch::Followed final : public net::ITimerHandler {
    PublisherWatch& watch_;

    enum class Wait : std::uint8_t {
        // The publisher is gone; at the end the stream ends unless it is back.
        Grace,
        RetryStart,
        RetryCheck,
    };

    Followed(PublisherWatch& watch, core::LiveStreamId stream) noexcept
        : watch_(watch), id(stream), key(stream.to_string()) {}
    ~Followed() override { cancel(); }
    Followed(const Followed&) = delete;
    Followed& operator=(const Followed&) = delete;

    void on_timeout() noexcept override {
        timer_.reset();
        watch_.on_timer(*this);
    }

    void arm(Wait what, core::Millis delay) noexcept {
        cancel();
        wait = what;
        timer_ = watch_.reactor_.arm_timer(delay, *this);
    }

    void cancel() noexcept {
        if (timer_) {
            watch_.reactor_.cancel_timer(*timer_);
            timer_.reset();
        }
    }

    [[nodiscard]] bool waiting() const noexcept { return timer_.has_value(); }
    [[nodiscard]] bool waiting_for(Wait what) const noexcept { return timer_ && wait == what; }
    [[nodiscard]] bool idle() const noexcept { return !timer_ && !starting && !checking; }

    const core::LiveStreamId id;
    const std::string key;
    std::optional<core::UserId> owner;
    // Sessions joined and not seen leaving, and sessions seen leaving.
    std::vector<std::string> present;
    std::vector<std::string> gone;
    Wait wait = Wait::Grace;
    bool starting = false;
    bool checking = false;
    // The grace ran out while a start was under way: look once it has answered.
    bool check_after_start = false;
    // Went live for the sessions present now; a repeat event for them asks nothing again.
    bool live = false;
    std::uint32_t start_attempts = 0;
    std::uint32_t check_attempts = 0;
    std::uint64_t used = 0;
    std::optional<net::TimerId> timer_;
};

PublisherWatch::PublisherWatch(net::IReactor& reactor, LiveStreams& live, ops::Logger& log,
                               WatchSettings settings)
    : reactor_(reactor), live_(live), log_(log), settings_(settings) {}

PublisherWatch::~PublisherWatch() {
    *alive_ = false;
}

bool PublisherWatch::in_grace(const core::LiveStreamId& id) const noexcept {
    const auto it = streams_.find(id.to_string());
    return it != streams_.end() && it->second->waiting_for(Followed::Wait::Grace);
}

PublisherWatch::Followed* PublisherWatch::find(const std::string& key) noexcept {
    const auto it = streams_.find(key);
    return it == streams_.end() ? nullptr : it->second.get();
}

PublisherWatch::Followed* PublisherWatch::follow(const core::LiveStreamId& id) noexcept {
    const std::string key = id.to_string();
    if (Followed* known = find(key)) {
        known->used = ++stamp_;
        return known;
    }
    if (streams_.size() >= settings_.max_streams) {
        // The idle stream heard of longest ago makes room; one with a wait or a call under way
        // is never dropped.
        auto oldest = streams_.end();
        std::uint64_t oldest_use = std::numeric_limits<std::uint64_t>::max();
        for (auto it = streams_.begin(); it != streams_.end(); ++it) {
            if (it->second->idle() && it->second->used < oldest_use) {
                oldest = it;
                oldest_use = it->second->used;
            }
        }
        if (oldest == streams_.end()) {
            ++counters_.untracked;
            log_.warn("live webhook for a stream not followed: too many followed",
                      {{"stream", key}, {"followed", streams_.size()}});
            return nullptr;
        }
        streams_.erase(oldest);
    }
    auto made = std::make_unique<Followed>(*this, id);
    made->used = ++stamp_;
    Followed* followed = made.get();
    streams_.emplace(key, std::move(made));
    return followed;
}

void PublisherWatch::forget(const std::string& key) noexcept {
    streams_.erase(key);
}

void PublisherWatch::on_event(const WebhookEvent& event) noexcept {
    ++counters_.events;
    switch (event.kind) {
    case WebhookEventKind::ParticipantJoined:
    case WebhookEventKind::TrackPublished:
    case WebhookEventKind::ParticipantLeft:
    case WebhookEventKind::ParticipantAborted: {
        const auto stream = stream_of_room(event.room);
        const auto owner = stream ? publisher_of(event.identity, *stream) : std::nullopt;
        if (!owner) {
            ++counters_.ignored;
            return;
        }
        Followed* followed = follow(*stream);
        if (followed == nullptr) {
            return;
        }
        if (event.kind == WebhookEventKind::ParticipantJoined ||
            event.kind == WebhookEventKind::TrackPublished) {
            joined(*followed, *owner, event.participant_sid);
        } else {
            left(*followed, event.participant_sid);
        }
        return;
    }
    case WebhookEventKind::RoomFinished: {
        const auto stream = stream_of_room(event.room);
        if (!stream) {
            ++counters_.ignored;
            return;
        }
        if (Followed* followed = follow(*stream)) {
            room_gone(*followed);
        }
        return;
    }
    case WebhookEventKind::Other:
        ++counters_.ignored;
        return;
    }
}

void PublisherWatch::joined(Followed& stream, const core::UserId& owner,
                            const std::string& session) noexcept {
    if (std::ranges::contains(stream.gone, session)) {
        ++counters_.stale;
        return;
    }
    stream.owner = owner;
    const bool known = std::ranges::contains(stream.present, session);
    if (!known) {
        stream.present.push_back(session);
    }
    if (stream.waiting_for(Followed::Wait::Grace) ||
        stream.waiting_for(Followed::Wait::RetryCheck)) {
        stream.cancel();
        ++counters_.returns;
        log_.info("live publisher back", {{"stream", stream.key}, {"session", session}});
    }
    stream.check_after_start = false;
    // The same session's further tracks, or the same event again, once it is live.
    if (known && stream.live) {
        return;
    }
    stream.start_attempts = 0;
    start(stream);
}

void PublisherWatch::left(Followed& stream, const std::string& session) noexcept {
    std::erase(stream.present, session);
    if (!std::ranges::contains(stream.gone, session)) {
        if (stream.gone.size() >= kGoneSessions) {
            stream.gone.erase(stream.gone.begin());
        }
        stream.gone.push_back(session);
    }
    if (!stream.present.empty() || stream.waiting_for(Followed::Wait::Grace)) {
        return;
    }
    stream.live = false;
    stream.check_attempts = 0;
    ++counters_.departures;
    log_.info(
        "live publisher left",
        {{"stream", stream.key}, {"session", session}, {"grace_ms", settings_.grace.count()}});
    stream.arm(Followed::Wait::Grace, settings_.grace);
}

void PublisherWatch::room_gone(Followed& stream) noexcept {
    for (std::string& session : stream.present) {
        if (stream.gone.size() >= kGoneSessions) {
            stream.gone.erase(stream.gone.begin());
        }
        stream.gone.push_back(std::move(session));
    }
    stream.present.clear();
    if (stream.waiting_for(Followed::Wait::Grace)) {
        return;
    }
    stream.live = false;
    stream.check_attempts = 0;
    ++counters_.departures;
    stream.arm(Followed::Wait::Grace, settings_.grace);
}

void PublisherWatch::on_timer(Followed& stream) noexcept {
    switch (stream.wait) {
    case Followed::Wait::RetryStart:
        if (!stream.present.empty()) {
            start(stream);
        }
        return;
    case Followed::Wait::Grace:
    case Followed::Wait::RetryCheck:
        check(stream);
        return;
    }
}

void PublisherWatch::start(Followed& stream) noexcept {
    if (stream.starting || !stream.owner) {
        return;
    }
    stream.starting = true;
    ++stream.start_attempts;
    ++counters_.starts;
    live_.go_live(stream.id, *stream.owner,
                  [this, alive = std::weak_ptr<bool>(alive_), key = stream.key](
                      std::expected<core::ports::LiveStream, LiveFailure> r) noexcept {
                      const auto held = alive.lock();
                      Followed* s = held && *held ? find(key) : nullptr;
                      if (s == nullptr) {
                          return;
                      }
                      s->starting = false;
                      if (r) {
                          s->live = !s->present.empty();
                          s->start_attempts = 0;
                      } else {
                          switch (r.error()) {
                          // Not ours to start (no such stream, or the identity's user is not its
                          // owner), or over: nothing more to follow.
                          case LiveFailure::NotFound:
                          case LiveFailure::Ended:
                              forget(key);
                              return;
                          case LiveFailure::Unavailable:
                              ++counters_.start_failures;
                              if (!s->present.empty() && s->start_attempts < settings_.attempts &&
                                  !s->waiting()) {
                                  s->arm(Followed::Wait::RetryStart, settings_.retry);
                              }
                              break;
                          case LiveFailure::Full:
                          case LiveFailure::Internal:
                              ++counters_.start_failures;
                              log_.warn("live stream not started from a webhook",
                                        {{"stream", key}, {"error", to_string(r.error())}});
                              break;
                          }
                      }
                      if (s->check_after_start) {
                          s->check_after_start = false;
                          if (s->present.empty() && !s->waiting()) {
                              check(*s);
                          }
                      }
                  });
}

void PublisherWatch::check(Followed& stream) noexcept {
    if (!stream.present.empty() || stream.checking) {
        return;
    }
    // A start under way would mark the stream live after this looked: look once it answers.
    if (stream.starting) {
        stream.check_after_start = true;
        return;
    }
    stream.checking = true;
    ++stream.check_attempts;
    live_.publisher_left(stream.id, [this, alive = std::weak_ptr<bool>(alive_), key = stream.key](
                                        std::expected<Departure, LiveFailure> r) noexcept {
        const auto held = alive.lock();
        Followed* s = held && *held ? find(key) : nullptr;
        if (s == nullptr) {
            return;
        }
        s->checking = false;
        if (r) {
            switch (*r) {
            case Departure::Ended:
                ++counters_.ended;
                forget(key);
                return;
            case Departure::Over:
                forget(key);
                return;
            case Departure::Present:
                ++counters_.kept;
                log_.info("live publisher still connected after its grace", {{"stream", key}});
                s->check_attempts = 0;
                return;
            case Departure::NotLive:
                s->check_attempts = 0;
                return;
            }
            return;
        }
        switch (r.error()) {
        case LiveFailure::NotFound:
        case LiveFailure::Ended:
            forget(key);
            return;
        case LiveFailure::Unavailable:
        case LiveFailure::Full:
        case LiveFailure::Internal:
            ++counters_.check_failures;
            if (s->present.empty() && s->check_attempts < settings_.attempts && !s->waiting()) {
                s->arm(Followed::Wait::RetryCheck, settings_.retry);
                return;
            }
            log_.warn("live publisher's departure left to the sweep",
                      {{"stream", key}, {"error", to_string(r.error())}});
            return;
        }
    });
}

} // namespace gateway
