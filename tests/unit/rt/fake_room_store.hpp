#pragma once

#include "rt/room_store.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ulw::test {

// Holds every call until the test answers it, so a test decides what the database said and
// when. Calls are answered in any order the test likes, as a real pool's sessions may.
class FakeRoomStore final : public rt::IRoomStore {
public:
    struct Resolve {
        core::RoomId room;
        rt::StoreCallback<rt::Ownership> done;
    };
    struct ClaimStale {
        std::vector<core::RoomId> rooms;
        rt::StoreCallback<std::vector<rt::OwnedRoom>> done;
    };
    struct Heartbeat {
        std::vector<rt::OwnedRoom> rooms;
        rt::StoreCallback<std::vector<core::RoomId>> done;
    };
    struct Append {
        core::RoomId room;
        std::uint64_t generation;
        rt::StoreCallback<std::optional<std::uint64_t>> done;
    };
    struct Lookup {
        core::NodeId node;
        rt::StoreCallback<std::optional<std::string>> done;
    };
    struct Release {
        std::vector<rt::OwnedRoom> rooms;
        rt::StoreCallback<void> done;
    };

    void watch(rt::IOwnershipListener& listener) noexcept override { listener_ = &listener; }

    void resolve(const core::RoomId& room, const core::NodeId& /*node*/,
                 rt::StoreCallback<rt::Ownership> done) override {
        resolves.push_back({.room = room, .done = std::move(done)});
    }
    void claim_stale(std::vector<core::RoomId> rooms, const core::NodeId& /*node*/,
                     rt::StoreCallback<std::vector<rt::OwnedRoom>> done) override {
        claims.push_back({.rooms = std::move(rooms), .done = std::move(done)});
    }
    void heartbeat(const core::NodeId& /*node*/, const core::Uuid& /*incarnation*/,
                   std::vector<rt::OwnedRoom> rooms,
                   rt::StoreCallback<std::vector<core::RoomId>> done) override {
        heartbeats.push_back({.rooms = std::move(rooms), .done = std::move(done)});
    }
    void append(const core::RoomId& room, std::uint64_t generation,
                rt::StoreCallback<std::optional<std::uint64_t>> done) override {
        appends.push_back({.room = room, .generation = generation, .done = std::move(done)});
    }
    void release(const core::NodeId& /*node*/, std::vector<rt::OwnedRoom> rooms,
                 rt::StoreCallback<void> done) override {
        releases.push_back({.rooms = std::move(rooms), .done = std::move(done)});
    }
    void advertise(const core::NodeId& /*node*/, std::string /*address*/,
                   const core::Uuid& /*incarnation*/, rt::StoreCallback<void> done) override {
        advertisements.push_back(std::move(done));
    }
    void find_address(const core::NodeId& node,
                      rt::StoreCallback<std::optional<std::string>> done) override {
        lookups.push_back({.node = node, .done = std::move(done)});
    }

    [[nodiscard]] rt::IOwnershipListener& listener() const { return *listener_; }

    template <class Call> static Call take(std::deque<Call>& calls) {
        Call call = std::move(calls.front());
        calls.pop_front();
        return call;
    }

    std::deque<Resolve> resolves;
    std::deque<ClaimStale> claims;
    std::deque<Heartbeat> heartbeats;
    std::deque<Append> appends;
    std::deque<Release> releases;
    std::deque<rt::StoreCallback<void>> advertisements;
    std::deque<Lookup> lookups;

private:
    rt::IOwnershipListener* listener_ = nullptr;
};

} // namespace ulw::test
