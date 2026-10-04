#pragma once

#include "core/ports/live.hpp"
#include "core/ports/media.hpp"
#include "net/reactor.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ulw::test {

// Runs calls on a later loop turn, as every port promises its callbacks run.
class LaterCalls final : public net::ITimerHandler {
public:
    explicit LaterCalls(net::IReactor& reactor) noexcept : reactor_(reactor) {}
    ~LaterCalls() override {
        if (armed_) {
            reactor_.cancel_timer(timer_);
        }
    }
    LaterCalls(const LaterCalls&) = delete;
    LaterCalls& operator=(const LaterCalls&) = delete;

    void post(std::move_only_function<void() noexcept> call) {
        pending_.push_back(std::move(call));
        if (!armed_) {
            timer_ = reactor_.arm_timer(core::Millis{0}, *this);
            armed_ = true;
        }
    }
    void on_timeout() noexcept override {
        armed_ = false;
        std::vector<std::move_only_function<void() noexcept>> due;
        due.swap(pending_);
        for (auto& call : due) {
            call();
        }
    }

private:
    net::IReactor& reactor_;
    std::vector<std::move_only_function<void() noexcept>> pending_;
    net::TimerId timer_{};
    bool armed_ = false;
};

// The rows, in memory, with the store's rules: one unfinished stream per owner, a cap on
// unfinished streams, states that only move forward.
class FakeLiveStore final : public core::ports::ILiveStreamStore {
public:
    using LiveStream = core::ports::LiveStream;
    using LiveStoreError = core::ports::LiveStoreError;

    explicit FakeLiveStore(net::IReactor& reactor) noexcept : later_(reactor) {}

    void create(core::ports::NewLiveStream stream, core::ports::LiveLimits limits,
                core::ports::LiveCallback<core::ports::CreatedLiveStream> done) override {
        ++calls;
        auto answer = [&]() -> core::ports::LiveResult<core::ports::CreatedLiveStream> {
            if (fail) {
                return std::unexpected(*fail);
            }
            std::int64_t unfinished = 0;
            std::int64_t recent = 0;
            for (const auto& [id, row] : rows) {
                if (row.owner == stream.owner &&
                    row.created_at > stream.at - std::chrono::hours(1)) {
                    ++recent;
                }
                if (row.state == core::ports::LiveState::Ended) {
                    continue;
                }
                if (row.owner == stream.owner) {
                    return core::ports::CreatedLiveStream{.stream = row, .created = false};
                }
                ++unfinished;
            }
            if (recent >= limits.per_owner_per_hour) {
                return std::unexpected(LiveStoreError::TooMany);
            }
            if (unfinished >= limits.max_unfinished) {
                return std::unexpected(LiveStoreError::Full);
            }
            LiveStream row{.id = stream.id,
                           .owner = stream.owner,
                           .state = core::ports::LiveState::Starting,
                           .passphrase = stream.passphrase,
                           .created_at = stream.at,
                           .live_at = std::nullopt,
                           .ended_at = std::nullopt,
                           .ended_by = std::nullopt,
                           .recording = std::nullopt};
            rows.insert_or_assign(stream.id.to_string(), row);
            return core::ports::CreatedLiveStream{.stream = row, .created = true};
        }();
        later_.post([done = std::move(done), answer = std::move(answer)]() mutable noexcept {
            done(std::move(answer));
        });
    }

    void find(const core::LiveStreamId& id, core::ports::LiveCallback<LiveStream> done) override {
        ++calls;
        reply(std::move(done), [&]() -> core::ports::LiveResult<LiveStream> { return row(id); });
    }

    void mark_live(const core::LiveStreamId& id, core::WallTime at,
                   core::ports::LiveCallback<LiveStream> done) override {
        ++calls;
        reply(std::move(done), [&]() -> core::ports::LiveResult<LiveStream> {
            auto found = row(id);
            if (!found) {
                return found;
            }
            LiveStream& r = rows.at(id.to_string());
            if (r.state != core::ports::LiveState::Ended) {
                r.state = core::ports::LiveState::Live;
                if (!r.live_at) {
                    r.live_at = at;
                }
            }
            return r;
        });
    }

    void end(const core::LiveStreamId& id, core::ports::LiveEnd reason, core::WallTime at,
             core::ports::LiveCallback<core::ports::EndedLiveStream> done) override {
        ++calls;
        reply(std::move(done), [&]() -> core::ports::LiveResult<core::ports::EndedLiveStream> {
            auto found = row(id);
            if (!found) {
                return std::unexpected(found.error());
            }
            LiveStream& r = rows.at(id.to_string());
            const bool ending = r.state != core::ports::LiveState::Ended;
            if (ending) {
                r.state = core::ports::LiveState::Ended;
                r.ended_at = at;
                r.ended_by = reason;
            }
            return core::ports::EndedLiveStream{.stream = r, .ended = ending};
        });
    }

    void unfinished(std::size_t limit,
                    core::ports::LiveCallback<std::vector<LiveStream>> done) override {
        ++calls;
        reply(std::move(done), [&]() -> core::ports::LiveResult<std::vector<LiveStream>> {
            std::vector<LiveStream> out;
            for (const auto& [id, r] : rows) {
                if (r.state != core::ports::LiveState::Ended && out.size() < limit) {
                    out.push_back(r);
                }
            }
            return out;
        });
    }

    // Every call fails with this while it is set.
    std::optional<LiveStoreError> fail;
    std::map<std::string, LiveStream> rows;
    std::size_t calls = 0;

private:
    [[nodiscard]] core::ports::LiveResult<LiveStream> row(const core::LiveStreamId& id) const {
        const auto it = rows.find(id.to_string());
        if (it == rows.end()) {
            return std::unexpected(LiveStoreError::NotFound);
        }
        return it->second;
    }

    template <class T, class Make> void reply(core::ports::LiveCallback<T> done, Make make) {
        core::ports::LiveResult<T> answer =
            fail ? core::ports::LiveResult<T>(std::unexpected(*fail)) : make();
        later_.post([done = std::move(done), answer = std::move(answer)]() mutable noexcept {
            done(std::move(answer));
        });
    }

    LaterCalls later_;
};

// What the service asked of the media server, one line per call, and answers it can be made
// to refuse.
class FakeSfu final : public core::ports::ISfu {
public:
    explicit FakeSfu(net::IReactor& reactor) noexcept : later_(reactor) {}

    void open_room(const core::RoomId& room, core::ports::MediaGeneration generation,
                   core::ports::MediaRoomKind kind, std::uint16_t max_participants,
                   OpenDone done) override {
        calls.push_back("open " + room.to_string() + ":" +
                        std::to_string(std::to_underlying(generation)) + " " +
                        (kind == core::ports::MediaRoomKind::Stream ? "stream" : "call") + " " +
                        std::to_string(max_participants));
        if (fail_open) {
            later_.post([done = std::move(done), e = *fail_open]() mutable noexcept {
                done(std::unexpected(e));
            });
            return;
        }
        auto handle = std::make_unique<Room>(*this, room.to_string());
        later_.post([done = std::move(done), handle = std::move(handle)]() mutable noexcept {
            done(std::move(handle));
        });
    }

    std::vector<std::string> calls;
    std::optional<core::ports::MediaError> fail_open;
    std::optional<core::ports::MediaError> fail_join;
    std::optional<core::ports::MediaError> fail_relay;
    std::optional<core::ports::MediaError> fail_close;
    // The adapter answers a relay already running with its id: one id per stream here.
    std::map<std::string, std::string> relays;
    std::size_t tickets = 0;
    core::WallTime ticket_expiry{std::chrono::seconds(1767225660)};

private:
    class Room final : public core::ports::IMediaRoom {
    public:
        Room(FakeSfu& sfu, std::string name) noexcept : sfu_(sfu), name_(std::move(name)) {}

        void join(const core::UserId& user, const core::DeviceId& device,
                  core::ports::MediaRole role, core::ports::TicketDone done) override {
            sfu_.calls.push_back(
                "join " + name_ + " " + std::string(user.view()) + "/" + device.to_string() + " " +
                (role == core::ports::MediaRole::Publisher ? "publisher" : "member"));
            std::expected<core::ports::MediaTicket, core::ports::MediaError> answer =
                sfu_.fail_join ? std::expected<core::ports::MediaTicket, core::ports::MediaError>(
                                     std::unexpected(*sfu_.fail_join))
                               : core::ports::MediaTicket{
                                     .endpoint = "https://media.example.test/whip/v1",
                                     .credential = "ticket-" + std::to_string(++sfu_.tickets),
                                     .expires_at = sfu_.ticket_expiry};
            sfu_.later_.post(
                [done = std::move(done), answer = std::move(answer)]() mutable noexcept {
                    done(std::move(answer));
                });
        }

        void participants(core::ports::ParticipantsDone done) override {
            sfu_.later_.post([done = std::move(done)]() mutable noexcept {
                done(std::unexpected(core::ports::MediaError::NotImplemented));
            });
        }

        void relay(const core::UserId& user, const core::DeviceId& device,
                   const core::ports::MediaRelay& target, core::ports::RelayDone done) override {
            sfu_.calls.push_back("relay " + name_ + " " + std::string(user.view()) + "/" +
                                 device.to_string() + " " + target.stream + " " +
                                 target.passphrase + " " +
                                 std::to_string(target.keyframe_interval.count()));
            std::expected<std::string, core::ports::MediaError> answer;
            if (sfu_.fail_relay) {
                answer = std::unexpected(*sfu_.fail_relay);
            } else {
                const auto [it, added] = sfu_.relays.try_emplace(
                    target.stream, "EG_" + std::to_string(sfu_.relays.size() + 1));
                answer = it->second;
            }
            sfu_.later_.post(
                [done = std::move(done), answer = std::move(answer)]() mutable noexcept {
                    done(std::move(answer));
                });
        }

        void close(core::ports::MediaDone done) override {
            sfu_.calls.push_back("close " + name_);
            std::expected<void, core::ports::MediaError> answer;
            if (sfu_.fail_close) {
                answer = std::unexpected(*sfu_.fail_close);
            }
            sfu_.later_.post([done = std::move(done), answer]() mutable noexcept { done(answer); });
        }

    private:
        FakeSfu& sfu_;
        std::string name_;
    };

    LaterCalls later_;
};

// Packagers that start in whatever state the test sets.
class FakePackagers final : public core::ports::IPackagers {
public:
    explicit FakePackagers(net::IReactor& reactor) noexcept : later_(reactor) {}

    void start(const core::ports::PackagerSpec& spec, core::ports::PackagerDone done) override {
        started.push_back(spec);
        std::expected<void, core::ports::PackagerError> answer;
        if (fail_start) {
            answer = std::unexpected(*fail_start);
        } else {
            states.try_emplace(spec.stream.to_string(), started_state);
        }
        later_.post([done = std::move(done), answer]() mutable noexcept { done(answer); });
    }

    void state(const core::LiveStreamId& stream, core::ports::PackagerStateDone done) override {
        ++state_calls;
        std::expected<core::ports::PackagerState, core::ports::PackagerError> answer;
        if (fail_state) {
            answer = std::unexpected(*fail_state);
        } else {
            const auto it = states.find(stream.to_string());
            answer = it == states.end() ? core::ports::PackagerState::Absent : it->second;
        }
        later_.post([done = std::move(done), answer]() mutable noexcept { done(answer); });
    }

    std::vector<core::ports::PackagerSpec> started;
    std::map<std::string, core::ports::PackagerState> states;
    core::ports::PackagerState started_state = core::ports::PackagerState::Ready;
    std::optional<core::ports::PackagerError> fail_start;
    std::optional<core::ports::PackagerError> fail_state;
    std::size_t state_calls = 0;

private:
    LaterCalls later_;
};

} // namespace ulw::test
