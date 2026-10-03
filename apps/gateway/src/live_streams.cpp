#include "live_streams.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace gateway {

namespace {

using core::ports::LiveEnd;
using core::ports::LiveState;
using core::ports::LiveStoreError;
using core::ports::MediaError;
using core::ports::PackagerError;
using core::ports::PackagerState;

// Every stream is generation 1 of its room for all its life: a stream is never moved on to a
// new generation, it ends (ADR-0092), and a closed generation is the stream's end.
constexpr core::ports::MediaGeneration kGeneration{1};
// 24 random bytes as hex: 48 characters, inside SRT's 10 to 79 (ADR-0046).
constexpr std::size_t kPassphraseBytes = 24;

// The stream's room, and the publisher's device: every ticket for a stream names the owner and
// the stream's own id, one identity, so a second session replaces the first (ADR-0053). The id
// is a UUID, so both parse.
core::RoomId room_of(const core::LiveStreamId& id) {
    return *core::RoomId::parse(id.to_string());
}

core::DeviceId device_of(const core::LiveStreamId& id) {
    return *core::DeviceId::parse(id.to_string());
}

std::string passphrase(core::ports::IRandom& random) {
    std::array<std::byte, kPassphraseBytes> bytes{};
    random.fill(bytes);
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(2 * bytes.size());
    for (const std::byte b : bytes) {
        const auto v = std::to_integer<unsigned>(b);
        out += kHex[v >> 4U];
        out += kHex[v & 0xFU];
    }
    return out;
}

std::size_t end_index(LiveEnd e) noexcept {
    return static_cast<std::size_t>(e);
}

} // namespace

std::string_view to_string(LiveFailure f) noexcept {
    switch (f) {
    case LiveFailure::NotFound:
        return "not found";
    case LiveFailure::Ended:
        return "ended";
    case LiveFailure::Full:
        return "full";
    case LiveFailure::RateLimited:
        return "rate limited";
    case LiveFailure::Unavailable:
        return "unavailable";
    case LiveFailure::Internal:
        return "internal";
    }
    return "internal";
}

// One wait on the reactor's clock. It is never destroyed from inside its own timer callback by
// anything but its owner's lazy reaping, which touches nothing of it afterwards.
class LiveStreams::Delay final : public net::ITimerHandler {
public:
    Delay(net::IReactor& reactor, core::Millis delay, std::move_only_function<void() noexcept> then)
        : reactor_(reactor), then_(std::move(then)) {
        timer_ = reactor_.arm_timer(delay, *this);
    }
    ~Delay() override {
        if (!fired_) {
            reactor_.cancel_timer(timer_);
        }
    }
    Delay(const Delay&) = delete;
    Delay& operator=(const Delay&) = delete;

    void on_timeout() noexcept override {
        fired_ = true;
        auto then = std::move(then_);
        // Nothing of this object is touched after the continuation, which may reap it.
        then();
    }

    [[nodiscard]] bool fired() const noexcept { return fired_; }

private:
    net::IReactor& reactor_;
    net::TimerId timer_;
    std::move_only_function<void() noexcept> then_;
    bool fired_ = false;
};

LiveStreams::LiveStreams(LiveDeps deps, LiveSettings settings) : deps_(deps), settings_(settings) {}

LiveStreams::~LiveStreams() {
    if (sweep_timer_) {
        deps_.reactor.cancel_timer(*sweep_timer_);
    }
}

LiveFailure LiveStreams::store_failure(LiveStoreError e) noexcept {
    switch (e) {
    case LiveStoreError::NotFound:
        return LiveFailure::NotFound;
    case LiveStoreError::Full:
        return LiveFailure::Full;
    case LiveStoreError::TooMany:
        return LiveFailure::RateLimited;
    case LiveStoreError::Unavailable:
        ++counters_.store_failures;
        return LiveFailure::Unavailable;
    case LiveStoreError::Corrupt:
        ++counters_.store_failures;
        return LiveFailure::Internal;
    }
    return LiveFailure::Internal;
}

LiveFailure LiveStreams::media_failure(MediaError e) noexcept {
    ++counters_.media_failures;
    switch (e) {
    case MediaError::Unavailable:
        return LiveFailure::Unavailable;
    // Only a handle closed through itself answers this, and every handle here is opened per
    // request; it means the stream's room was closed, which is its end.
    case MediaError::Closed:
        return LiveFailure::Ended;
    case MediaError::Refused:
    case MediaError::NotImplemented:
        return LiveFailure::Internal;
    }
    return LiveFailure::Internal;
}

LiveFailure LiveStreams::packager_failure(PackagerError e) noexcept {
    ++counters_.packager_failures;
    switch (e) {
    case PackagerError::Unavailable:
        return LiveFailure::Unavailable;
    // The cluster's quota, the platform's own bound: answered as the stream count's is.
    case PackagerError::Full:
        return LiveFailure::Full;
    case PackagerError::Refused:
        return LiveFailure::Internal;
    }
    return LiveFailure::Internal;
}

void LiveStreams::after(core::Millis delay, std::move_only_function<void() noexcept> then) {
    reap_delays();
    delays_.push_back(std::make_unique<Delay>(deps_.reactor, delay, std::move(then)));
}

void LiveStreams::reap_delays() noexcept {
    std::erase_if(delays_, [](const std::unique_ptr<Delay>& d) { return d->fired(); });
}

void LiveStreams::create(const core::UserId& owner, LiveDone<StartedStream> done) {
    ++pending_;
    core::ports::NewLiveStream row{
        .id = core::LiveStreamId::generate(deps_.clock, deps_.random),
        .owner = owner,
        .passphrase = passphrase(deps_.random),
        .at = deps_.clock.wall_now(),
    };
    deps_.store.create(
        std::move(row),
        core::ports::LiveLimits{.max_unfinished = settings_.max_streams,
                                .per_owner_per_hour = settings_.streams_per_user_per_hour},
        [this, done = std::move(done)](
            core::ports::LiveResult<core::ports::CreatedLiveStream> created) mutable noexcept {
            if (!created) {
                --pending_;
                done(std::unexpected(store_failure(created.error())));
                return;
            }
            if (created->created) {
                ++counters_.created;
                deps_.log.info("live stream created", {{"stream", created->stream.id.to_string()},
                                                       {"owner", created->stream.owner.view()}});
            }
            Stream stream = created->stream;
            const bool fresh = created->created;
            issue_ticket(
                stream,
                [this, stream, fresh, done = std::move(done)](
                    std::expected<core::ports::MediaTicket, LiveFailure> ticket) mutable noexcept {
                    --pending_;
                    if (!ticket) {
                        done(std::unexpected(ticket.error()));
                        return;
                    }
                    done(StartedStream{.stream = std::move(stream),
                                       .ticket = std::move(*ticket),
                                       .created = fresh});
                });
        });
}

void LiveStreams::find_owned(const core::LiveStreamId& id, const core::UserId& owner,
                             LiveDone<Stream> done) {
    deps_.store.find(id, [this, owner, done = std::move(done)](
                             core::ports::LiveResult<Stream> found) mutable noexcept {
        if (!found) {
            done(std::unexpected(store_failure(found.error())));
            return;
        }
        // Someone else's stream reads as no stream: its id is no reason to learn it exists.
        if (!(found->owner == owner)) {
            done(std::unexpected(LiveFailure::NotFound));
            return;
        }
        done(std::move(*found));
    });
}

void LiveStreams::ticket(const core::LiveStreamId& id, const core::UserId& owner,
                         LiveDone<core::ports::MediaTicket> done) {
    ++pending_;
    find_owned(
        id, owner,
        [this, done = std::move(done)](std::expected<Stream, LiveFailure> found) mutable noexcept {
            if (!found) {
                --pending_;
                done(std::unexpected(found.error()));
                return;
            }
            // A ticket only while the stream is the owner's current one: an ended
            // stream cannot be touched with anything it handed out (ADR-0053).
            if (found->state == LiveState::Ended) {
                --pending_;
                done(std::unexpected(LiveFailure::Ended));
                return;
            }
            issue_ticket(*found, [this, done = std::move(done)](auto ticket) mutable noexcept {
                --pending_;
                done(std::move(ticket));
            });
        });
}

void LiveStreams::issue_ticket(const Stream& stream, LiveDone<core::ports::MediaTicket> done) {
    deps_.sfu.open_room(
        room_of(stream.id), kGeneration, core::ports::MediaRoomKind::Stream, 0,
        [this, owner = stream.owner, device = device_of(stream.id),
         done = std::move(done)](std::expected<std::unique_ptr<core::ports::IMediaRoom>, MediaError>
                                     opened) mutable noexcept {
            if (!opened) {
                done(std::unexpected(media_failure(opened.error())));
                return;
            }
            core::ports::IMediaRoom* room = opened->get();
            room->join(
                owner, device, core::ports::MediaRole::Publisher,
                [this, held = std::move(*opened), done = std::move(done)](
                    std::expected<core::ports::MediaTicket, MediaError> ticket) mutable noexcept {
                    held.reset();
                    if (!ticket) {
                        done(std::unexpected(media_failure(ticket.error())));
                        return;
                    }
                    ++counters_.tickets;
                    done(std::move(*ticket));
                });
        });
}

void LiveStreams::go_live(const core::LiveStreamId& id, const core::UserId& owner,
                          LiveDone<Stream> done) {
    ++pending_;
    LiveDone<Stream> finished =
        [this, done = std::move(done)](std::expected<Stream, LiveFailure> r) mutable noexcept {
            --pending_;
            done(std::move(r));
        };
    find_owned(id, owner,
               [this, finished = std::move(finished)](
                   std::expected<Stream, LiveFailure> found) mutable noexcept {
                   if (!found) {
                       finished(std::unexpected(found.error()));
                       return;
                   }
                   if (found->state == LiveState::Ended) {
                       finished(std::unexpected(LiveFailure::Ended));
                       return;
                   }
                   Stream stream = std::move(*found);
                   const core::ports::PackagerSpec spec{
                       .stream = stream.id, .owner = stream.owner, .passphrase = stream.passphrase};
                   deps_.packagers.start(
                       spec, [this, stream = std::move(stream), finished = std::move(finished)](
                                 std::expected<void, PackagerError> started) mutable noexcept {
                           if (!started) {
                               deps_.log.warn("live packager did not start",
                                              {{"stream", stream.id.to_string()},
                                               {"error", core::ports::to_string(started.error())}});
                               finished(std::unexpected(packager_failure(started.error())));
                               return;
                           }
                           await_packager(std::move(stream),
                                          deps_.reactor.now() + settings_.ready_wait,
                                          std::move(finished));
                       });
               });
}

void LiveStreams::await_packager(Stream stream, core::MonoTime deadline, LiveDone<Stream> done) {
    const core::LiveStreamId id = stream.id;
    deps_.packagers.state(
        id, [this, stream = std::move(stream), deadline, done = std::move(done)](
                std::expected<PackagerState, PackagerError> state) mutable noexcept {
            if (!state) {
                done(std::unexpected(packager_failure(state.error())));
                return;
            }
            switch (*state) {
            case PackagerState::Ready:
                relay(std::move(stream), std::move(done));
                return;
            // A packager that has already gone ran its stream to an end, or could not run it: the
            // stream cannot go live again (ADR-0047 refuses to restart an ended stream).
            case PackagerState::Finished:
            case PackagerState::Failed: {
                const LiveEnd reason =
                    *state == PackagerState::Finished ? LiveEnd::Finished : LiveEnd::Failed;
                finish(
                    stream.id, reason,
                    [done = std::move(done)](std::expected<Stream, LiveFailure>) mutable noexcept {
                        done(std::unexpected(LiveFailure::Ended));
                    });
                return;
            }
            case PackagerState::Absent:
            case PackagerState::Starting:
                break;
            }
            if (deps_.reactor.now() >= deadline) {
                ++counters_.packager_failures;
                deps_.log.warn("live packager not listening in time",
                               {{"stream", stream.id.to_string()}});
                done(std::unexpected(LiveFailure::Unavailable));
                return;
            }
            after(settings_.ready_poll, [this, stream = std::move(stream), deadline,
                                         done = std::move(done)]() mutable noexcept {
                await_packager(std::move(stream), deadline, std::move(done));
            });
        });
}

void LiveStreams::relay(Stream stream, LiveDone<Stream> done) {
    const core::RoomId room_id = room_of(stream.id);
    deps_.sfu.open_room(
        room_id, kGeneration, core::ports::MediaRoomKind::Stream, 0,
        [this, stream = std::move(stream),
         done = std::move(done)](std::expected<std::unique_ptr<core::ports::IMediaRoom>, MediaError>
                                     opened) mutable noexcept {
            if (!opened) {
                done(std::unexpected(media_failure(opened.error())));
                return;
            }
            core::ports::IMediaRoom* room = opened->get();
            const core::ports::MediaRelay target{.stream = stream.id.to_string(),
                                                 .passphrase = stream.passphrase,
                                                 .keyframe_interval = settings_.segment};
            const core::UserId owner = stream.owner;
            const core::DeviceId device = device_of(stream.id);
            room->relay(
                owner, device, target,
                [this, held = std::move(*opened), stream = std::move(stream),
                 done = std::move(done)](
                    std::expected<std::string, MediaError> relayed) mutable noexcept {
                    held.reset();
                    if (!relayed) {
                        deps_.log.warn("live relay refused",
                                       {{"stream", stream.id.to_string()},
                                        {"error", core::ports::to_string(relayed.error())}});
                        done(std::unexpected(media_failure(relayed.error())));
                        return;
                    }
                    const bool first = stream.state == LiveState::Starting;
                    deps_.store.mark_live(
                        stream.id, deps_.clock.wall_now(),
                        [this, first, relay_id = std::move(*relayed), done = std::move(done)](
                            core::ports::LiveResult<Stream> marked) mutable noexcept {
                            if (!marked) {
                                done(std::unexpected(store_failure(marked.error())));
                                return;
                            }
                            // Ended meanwhile: the owner's end, or a sweep's, whose close may
                            // have come before this relay. The room is closed again, so the
                            // relay just started ends with it.
                            if (marked->state == LiveState::Ended) {
                                close_room(
                                    std::move(*marked),
                                    [done = std::move(done)](
                                        std::expected<Stream, LiveFailure>) mutable noexcept {
                                        done(std::unexpected(LiveFailure::Ended));
                                    });
                                return;
                            }
                            if (first) {
                                ++counters_.went_live;
                                deps_.log.info(
                                    "live stream relayed",
                                    {{"stream", marked->id.to_string()}, {"relay", relay_id}});
                            }
                            done(std::move(*marked));
                        });
                });
        });
}

void LiveStreams::end(const core::LiveStreamId& id, const core::UserId& owner,
                      LiveDone<Stream> done) {
    ++pending_;
    find_owned(
        id, owner,
        [this, done = std::move(done)](std::expected<Stream, LiveFailure> found) mutable noexcept {
            if (!found) {
                --pending_;
                done(std::unexpected(found.error()));
                return;
            }
            finish(found->id, LiveEnd::Owner,
                   [this,
                    done = std::move(done)](std::expected<Stream, LiveFailure> r) mutable noexcept {
                       --pending_;
                       done(std::move(r));
                   });
        });
}

void LiveStreams::finish(const core::LiveStreamId& id, LiveEnd reason, LiveDone<Stream> done) {
    deps_.store.end(
        id, reason, deps_.clock.wall_now(),
        [this, reason, done = std::move(done)](
            core::ports::LiveResult<core::ports::EndedLiveStream> ended) mutable noexcept {
            if (!ended) {
                done(std::unexpected(store_failure(ended.error())));
                return;
            }
            // Counted and logged for the end that took, not for a repeat.
            if (ended->ended) {
                ++counters_.ended.at(end_index(reason));
                deps_.log.info("live stream ended", {{"stream", ended->stream.id.to_string()},
                                                     {"reason", core::ports::to_string(reason)}});
            }
            close_room(std::move(ended->stream), std::move(done));
        });
}

// Closing the generation removes the publisher; its relay ends with it, which closes the SRT
// session, and the packager ends the playlist and records the stream (ADR-0053). A packager
// that no relay ever reached ends the stream itself once its wait for a caller runs out.
void LiveStreams::close_room(Stream stream, LiveDone<Stream> done) {
    const core::RoomId room_id = room_of(stream.id);
    deps_.sfu.open_room(
        room_id, kGeneration, core::ports::MediaRoomKind::Stream, 0,
        [this, stream = std::move(stream),
         done = std::move(done)](std::expected<std::unique_ptr<core::ports::IMediaRoom>, MediaError>
                                     opened) mutable noexcept {
            if (!opened) {
                done(std::unexpected(media_failure(opened.error())));
                return;
            }
            core::ports::IMediaRoom* room = opened->get();
            room->close(
                [this, held = std::move(*opened), stream = std::move(stream),
                 done = std::move(done)](std::expected<void, MediaError> closed) mutable noexcept {
                    held.reset();
                    if (!closed) {
                        done(std::unexpected(media_failure(closed.error())));
                        return;
                    }
                    done(std::move(stream));
                });
        });
}

void LiveStreams::status(const core::LiveStreamId& id, LiveDone<Stream> done) {
    ++pending_;
    deps_.store.find(
        id, [this, done = std::move(done)](core::ports::LiveResult<Stream> found) mutable noexcept {
            --pending_;
            if (!found) {
                done(std::unexpected(store_failure(found.error())));
                return;
            }
            done(std::move(*found));
        });
}

void LiveStreams::publisher_left(const core::LiveStreamId& id, LiveDone<Departure> done) {
    ++pending_;
    LiveDone<Departure> finished =
        [this, done = std::move(done)](std::expected<Departure, LiveFailure> r) mutable noexcept {
            --pending_;
            done(r);
        };
    deps_.store.find(id, [this, finished = std::move(finished)](
                             core::ports::LiveResult<Stream> found) mutable noexcept {
        if (!found) {
            finished(std::unexpected(store_failure(found.error())));
            return;
        }
        // A starting stream keeps its start window: a broadcaster may take a while to set an
        // encoder up, and its room comes and goes with the tickets meanwhile.
        if (found->state != LiveState::Live) {
            finished(found->state == LiveState::Ended ? Departure::Over : Departure::NotLive);
            return;
        }
        const core::LiveStreamId stream = found->id;
        deps_.sfu.present(
            room_of(stream), kGeneration, found->owner, device_of(stream),
            [this, stream, finished = std::move(finished)](
                std::expected<bool, MediaError> present) mutable noexcept {
                if (!present) {
                    finished(std::unexpected(media_failure(present.error())));
                    return;
                }
                if (*present) {
                    finished(Departure::Present);
                    return;
                }
                deps_.store.end(
                    stream, LiveEnd::PublisherLeft, deps_.clock.wall_now(),
                    [this, finished = std::move(finished)](
                        core::ports::LiveResult<core::ports::EndedLiveStream>
                            ended) mutable noexcept {
                        if (!ended) {
                            finished(std::unexpected(store_failure(ended.error())));
                            return;
                        }
                        // Someone else's end got there first: the owner's, or the sweep's.
                        if (!ended->ended) {
                            finished(Departure::Over);
                            return;
                        }
                        ++counters_.ended.at(end_index(LiveEnd::PublisherLeft));
                        deps_.log.info(
                            "live stream ended",
                            {{"stream", ended->stream.id.to_string()},
                             {"reason", core::ports::to_string(LiveEnd::PublisherLeft)}});
                        // The room goes too, so no ticket still in a client's hands brings it
                        // back; the row has ended whether or not that works.
                        close_room(std::move(ended->stream),
                                   [finished = std::move(finished)](
                                       std::expected<Stream, LiveFailure>) mutable noexcept {
                                       finished(Departure::Ended);
                                   });
                    });
            });
    });
}

void LiveStreams::playlist_ended(const core::LiveStreamId& id) {
    std::string key = id.to_string();
    if (ending_.contains(key)) {
        return;
    }
    ending_.insert(key);
    ++pending_;
    finish(id, LiveEnd::Finished,
           [this, key = std::move(key)](std::expected<Stream, LiveFailure>) noexcept {
               --pending_;
               ending_.erase(key);
           });
}

void LiveStreams::start_sweeping() noexcept {
    if (!sweep_timer_) {
        sweep_timer_ = deps_.reactor.arm_timer(settings_.sweep_interval, *this);
    }
}

void LiveStreams::sweep_now() noexcept {
    if (sweep_timer_) {
        deps_.reactor.cancel_timer(*sweep_timer_);
        sweep_timer_.reset();
    }
    reap_delays();
    sweep();
}

void LiveStreams::on_timeout() noexcept {
    sweep_timer_.reset();
    reap_delays();
    sweep();
}

void LiveStreams::sweep() noexcept {
    if (sweeping_) {
        return;
    }
    sweeping_ = true;
    ++counters_.sweeps;
    deps_.store.unfinished(settings_.sweep_batch,
                           [this](core::ports::LiveResult<std::vector<Stream>> streams) noexcept {
                               if (!streams) {
                                   static_cast<void>(store_failure(streams.error()));
                                   sweep_done();
                                   return;
                               }
                               sweep_list_ = std::move(*streams);
                               sweep_index_ = 0;
                               sweep_next();
                           });
}

// One stream at a time: a sweep is a background chore, and its calls to the packager runtime
// and the media server should never come in a burst.
void LiveStreams::sweep_next() noexcept {
    if (sweep_index_ >= sweep_list_.size()) {
        sweep_done();
        return;
    }
    const Stream stream = sweep_list_[sweep_index_++];
    sweep_one(stream, [this]() noexcept { sweep_next(); });
}

void LiveStreams::sweep_one(const Stream& stream, std::move_only_function<void() noexcept> next) {
    const core::WallTime now = deps_.clock.wall_now();
    LiveDone<Stream> then =
        [next = std::move(next)](std::expected<Stream, LiveFailure>) mutable noexcept { next(); };
    if (stream.state == LiveState::Starting) {
        if (now - stream.created_at > settings_.start_window) {
            finish(stream.id, LiveEnd::Timeout, std::move(then));
            return;
        }
        // Nothing to look at until it goes live; going live checks its own packager.
        after(core::Millis{0}, [then = std::move(then)]() mutable noexcept {
            then(std::unexpected(LiveFailure::Unavailable));
        });
        return;
    }
    if (stream.live_at && now - *stream.live_at > settings_.max_age) {
        finish(stream.id, LiveEnd::Timeout, std::move(then));
        return;
    }
    deps_.packagers.state(stream.id,
                          [this, id = stream.id, then = std::move(then)](
                              std::expected<PackagerState, PackagerError> state) mutable noexcept {
                              if (!state) {
                                  then(std::unexpected(packager_failure(state.error())));
                                  return;
                              }
                              switch (*state) {
                              case PackagerState::Finished:
                                  finish(id, LiveEnd::Finished, std::move(then));
                                  return;
                              // A live stream's packager gone without a trace failed as surely as
                              // one that says so.
                              case PackagerState::Failed:
                              case PackagerState::Absent:
                                  finish(id, LiveEnd::Failed, std::move(then));
                                  return;
                              case PackagerState::Starting:
                              case PackagerState::Ready:
                                  break;
                              }
                              then(std::unexpected(LiveFailure::Unavailable));
                          });
}

void LiveStreams::sweep_done() noexcept {
    sweeping_ = false;
    sweep_list_.clear();
    start_sweeping();
}

} // namespace gateway
