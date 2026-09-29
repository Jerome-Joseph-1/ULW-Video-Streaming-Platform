#pragma once

#include "net/reactor.hpp"
#include "rt/room_store.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ulw::test {

// The rows of migrations/0003_rooms.sql, shared by the stores of several in-process nodes.
// Staleness is set by the test rather than by a clock: a room is stale from make_stale() until
// its owner's next heartbeat or a claim.
class MemoryRooms {
public:
    struct Room {
        core::NodeId owner;
        std::uint64_t generation = 1;
        std::uint64_t last_seq = 0;
        bool stale = false;
    };

    // A listener's notices go through its own store, which defers them as a session would.
    using Notify = std::function<void(const core::RoomId&, const rt::Ownership&)>;

    void subscribe(Notify notify) { notices_.push_back(std::move(notify)); }

    std::unordered_map<core::RoomId, Room> rooms;
    std::unordered_map<std::string, std::string> addresses;
    // Which incarnation holds each name. A test erases one to model its holder going quiet.
    std::unordered_map<std::string, core::Uuid> holders;
    // Every append that matched no row, as (room, generation).
    std::vector<std::pair<core::RoomId, std::uint64_t>> refused_appends;

    void make_stale(const core::RoomId& room) { rooms.at(room).stale = true; }

    // Hands the room to `node` under the next generation, as its claim would.
    rt::Ownership take(const core::RoomId& room, const core::NodeId& node) {
        Room& r = rooms.at(room);
        r.owner = node;
        ++r.generation;
        r.stale = false;
        const rt::Ownership owner{.node = node, .generation = r.generation};
        announce(room, owner);
        return owner;
    }

    void announce(const core::RoomId& room, const rt::Ownership& owner) {
        for (const Notify& n : notices_) {
            n(room, owner);
        }
    }

private:
    std::vector<Notify> notices_;
};

// One node's IRoomStore over MemoryRooms. Answers and notices wait for the next loop
// iteration, never arriving inside the call.
class MemoryRoomStore final : public rt::IRoomStore, public net::ITimerHandler {
public:
    MemoryRoomStore(net::IReactor& reactor, MemoryRooms& db) : reactor_(reactor), db_(db) {
        db_.subscribe([this](const core::RoomId& room, const rt::Ownership& owner) {
            later([this, room, owner] {
                if (listener_ != nullptr && !deaf) {
                    listener_->on_owner_changed(room, owner);
                }
            });
        });
    }
    ~MemoryRoomStore() override { reactor_.cancel_timer(timer_); }
    MemoryRoomStore(const MemoryRoomStore&) = delete;
    MemoryRoomStore& operator=(const MemoryRoomStore&) = delete;
    MemoryRoomStore(MemoryRoomStore&&) = delete;
    MemoryRoomStore& operator=(MemoryRoomStore&&) = delete;

    // While false, every call fails as it would with the database out of reach, and nothing
    // it asked for happens.
    bool reachable = true;
    // While true, ownership notices are lost on the way, as over a dead listening session.
    bool deaf = false;

    void watch(rt::IOwnershipListener& listener) noexcept override { listener_ = &listener; }
    // For a node about to be destroyed while the others carry on.
    void unwatch() noexcept { listener_ = nullptr; }

    void resolve(const core::RoomId& room, const core::NodeId& node,
                 rt::StoreCallback<rt::Ownership> done) override {
        answer("", std::move(done), [this, room, node]() -> rt::StoreResult<rt::Ownership> {
            const auto it = db_.rooms.find(room);
            if (it == db_.rooms.end()) {
                db_.rooms.emplace(room, MemoryRooms::Room{.owner = node});
                const rt::Ownership created{.node = node, .generation = 1};
                db_.announce(room, created);
                return created;
            }
            if (it->second.owner == node || it->second.stale) {
                return db_.take(room, node);
            }
            return rt::Ownership{.node = it->second.owner, .generation = it->second.generation};
        });
    }

    void claim_stale(std::vector<core::RoomId> rooms, const core::NodeId& node,
                     rt::StoreCallback<std::vector<rt::OwnedRoom>> done) override {
        answer("", std::move(done), [this, rooms = std::move(rooms), node] {
            std::vector<rt::OwnedRoom> claimed;
            for (const core::RoomId& room : rooms) {
                const auto it = db_.rooms.find(room);
                if (it != db_.rooms.end() && it->second.stale) {
                    claimed.push_back(
                        {.room = room, .generation = db_.take(room, node).generation});
                }
            }
            return rt::StoreResult<std::vector<rt::OwnedRoom>>{claimed};
        });
    }

    void heartbeat(const core::NodeId& node, const core::Uuid& /*incarnation*/,
                   std::vector<rt::OwnedRoom> rooms,
                   rt::StoreCallback<std::vector<core::RoomId>> done) override {
        answer("heartbeat", std::move(done), [this, node, rooms = std::move(rooms)] {
            std::vector<core::RoomId> renewed;
            for (const rt::OwnedRoom& held : rooms) {
                const auto it = db_.rooms.find(held.room);
                if (it != db_.rooms.end() && it->second.owner == node &&
                    it->second.generation == held.generation) {
                    it->second.stale = false;
                    renewed.push_back(held.room);
                }
            }
            return rt::StoreResult<std::vector<core::RoomId>>{renewed};
        });
    }

    void append(const core::RoomId& room, std::uint64_t generation,
                rt::StoreCallback<std::optional<std::uint64_t>> done) override {
        answer("", std::move(done), [this, room, generation] {
            MemoryRooms::Room& r = db_.rooms.at(room);
            if (r.generation != generation) {
                db_.refused_appends.emplace_back(room, generation);
                return rt::StoreResult<std::optional<std::uint64_t>>{std::nullopt};
            }
            return rt::StoreResult<std::optional<std::uint64_t>>{++r.last_seq};
        });
    }

    void release(const core::NodeId& node, std::vector<rt::OwnedRoom> rooms,
                 rt::StoreCallback<void> done) override {
        answer("", std::move(done), [this, node, rooms = std::move(rooms)] {
            for (const rt::OwnedRoom& held : rooms) {
                const auto it = db_.rooms.find(held.room);
                if (it != db_.rooms.end() && it->second.owner == node &&
                    it->second.generation == held.generation) {
                    it->second.stale = true;
                }
            }
            return rt::StoreResult<void>{};
        });
    }

    void advertise(const core::NodeId& node, std::string address, const core::Uuid& incarnation,
                   rt::StoreCallback<void> done) override {
        answer("", std::move(done), [this, node, address = std::move(address), incarnation] {
            if (refuse_advertise) {
                return rt::StoreResult<void>{std::unexpected(rt::StoreError::Unavailable)};
            }
            const std::string name(node.view());
            const auto [holder, fresh] = db_.holders.try_emplace(name, incarnation);
            if (!fresh && holder->second != incarnation) {
                return rt::StoreResult<void>{std::unexpected(rt::StoreError::NodeTaken)};
            }
            db_.addresses.insert_or_assign(name, address);
            return rt::StoreResult<void>{};
        });
    }

    void find_address(const core::NodeId& node,
                      rt::StoreCallback<std::optional<std::string>> done) override {
        answer("", std::move(done), [this, node] {
            const auto it = db_.addresses.find(std::string(node.view()));
            return rt::StoreResult<std::optional<std::string>>{
                it == db_.addresses.end() ? std::nullopt : std::optional(it->second)};
        });
    }

    // While set, every advertise fails as if the database were down.
    bool refuse_advertise = false;

    // While set, answers and notices wait, as behind a database that has stopped answering
    // without dropping the connection.
    bool hold = false;

    // Lets what waited go, oldest first, except that answers to `first` ("heartbeat") go
    // before all others, as they may when the pool runs them on another session.
    void release_held(std::string_view first = {}) {
        hold = false;
        std::ranges::stable_partition(queue_, [&](const Call& c) { return c.what == first; });
        if (!armed_) {
            armed_ = true;
            timer_ = reactor_.arm_timer(core::Millis{0}, *this);
        }
    }

    [[nodiscard]] std::size_t waiting() const noexcept { return queue_.size(); }
    [[nodiscard]] std::size_t waiting(std::string_view what) const noexcept {
        return static_cast<std::size_t>(
            std::ranges::count_if(queue_, [&](const Call& c) { return c.what == what; }));
    }

    void on_timeout() noexcept override {
        armed_ = false;
        if (hold) {
            return;
        }
        std::vector<Call> due;
        due.swap(queue_);
        for (auto& call : due) {
            call.run();
        }
    }

private:
    template <class T, class Work>
    void answer(std::string_view what, rt::StoreCallback<T> done, Work work) {
        // The callback is move-only and std::function copies, so it waits in a shared slot.
        auto slot = std::make_shared<rt::StoreCallback<T>>(std::move(done));
        if (!reachable) {
            later([slot] { (*slot)(std::unexpected(rt::StoreError::Unavailable)); }, what);
            return;
        }
        later([slot, work = std::move(work)]() mutable { (*slot)(work()); }, what);
    }

    void later(std::function<void()> call, std::string_view what = {}) {
        queue_.push_back({.what = what, .run = std::move(call)});
        if (!armed_) {
            armed_ = true;
            timer_ = reactor_.arm_timer(core::Millis{0}, *this);
        }
    }

    net::IReactor& reactor_;
    MemoryRooms& db_;
    rt::IOwnershipListener* listener_ = nullptr;
    struct Call {
        std::string_view what;
        std::function<void()> run;
    };
    std::vector<Call> queue_;
    net::TimerId timer_;
    bool armed_ = false;
};

} // namespace ulw::test
