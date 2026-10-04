#include "rt/registry.hpp"

#include <algorithm>
#include <utility>

namespace rt {

RoomRegistry::RoomRegistry(IRoomStore& store, const core::ports::IClock& clock, core::NodeId self,
                           const core::Uuid& incarnation, IRegistryObserver& observer)
    : store_(store), clock_(clock), self_(self), incarnation_(incarnation), observer_(observer) {
    store_.watch(*this);
}

std::optional<std::uint64_t> RoomRegistry::owned(const core::RoomId& room) const noexcept {
    const auto it = owned_.find(room);
    if (it == owned_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::uint64_t RoomRegistry::taken_at(const core::RoomId& room) const noexcept {
    const auto it = taken_at_.find(room);
    return it == taken_at_.end() ? 0 : it->second;
}

void RoomRegistry::disown(const core::RoomId& room) noexcept {
    owned_.erase(room);
    taken_at_.erase(room);
}

std::optional<Ownership> RoomRegistry::known_owner(const core::RoomId& room) const noexcept {
    if (const auto generation = owned(room)) {
        return Ownership{.node = self_, .generation = *generation};
    }
    const auto it = cache_.find(room);
    if (it == cache_.end() || it->second.node == self_) {
        return std::nullopt;
    }
    return it->second;
}

void RoomRegistry::resolve(const core::RoomId& room, StoreCallback<Ownership> done) {
    auto [it, first] = resolving_.try_emplace(room);
    it->second.push_back(std::move(done));
    if (!first) {
        return;
    }
    store_.resolve(room, self_, [this, room](StoreResult<Ownership> result) noexcept {
        resolved(room, std::move(result));
    });
}

void RoomRegistry::resolved(const core::RoomId& room, StoreResult<Ownership> result) {
    const auto it = resolving_.find(room);
    if (it == resolving_.end()) {
        return;
    }
    std::vector<StoreCallback<Ownership>> waiting = std::move(it->second);
    resolving_.erase(it);
    if (result) {
        if (result->node == self_) {
            acquired(room, result->generation, result->last_seq);
        } else if (learn(room, *result)) {
            observer_.on_owner_changed(room, *result);
        }
    }
    for (auto& done : waiting) {
        done(result);
    }
}

void RoomRegistry::acquired(const core::RoomId& room, std::uint64_t generation,
                            std::uint64_t last_seq) {
    const auto previous = owned(room);
    if (previous && *previous >= generation) {
        return;
    }
    owned_[room] = generation;
    taken_at_[room] = last_seq;
    // Generation 1 is the room's creation; any later one took the room from an earlier owner.
    if (generation > 1) {
        ++counters_.reassignments;
    }
    const Ownership mine{.node = self_, .generation = generation};
    learn(room, mine);
    observer_.on_owner_changed(room, mine);
}

bool RoomRegistry::learn(const core::RoomId& room, const Ownership& owner) {
    const auto [it, inserted] = cache_.try_emplace(room, owner);
    if (inserted) {
        return true;
    }
    if (it->second.generation > owner.generation || it->second == owner) {
        return false;
    }
    it->second = owner;
    return true;
}

void RoomRegistry::forget(const core::RoomId& room) noexcept {
    const auto it = cache_.find(room);
    if (it != cache_.end() && it->second.node != self_) {
        cache_.erase(it);
    }
}

bool RoomRegistry::append(const core::RoomId& room, const Outgoing& message, AppendCallback done) {
    const auto generation = owned(room);
    if (!generation) {
        return false;
    }
    store_.append(
        room, *generation, message,
        [this, room, generation = *generation,
         done = std::move(done)](StoreResult<std::optional<std::uint64_t>> r) mutable noexcept {
            if (!r) {
                done(std::unexpected(r.error() == StoreError::Conflict ? AppendError::Conflict
                                                                       : AppendError::Unavailable));
                return;
            }
            if (!*r) {
                fenced(room, generation, OwnerWrite::Append);
                done(std::unexpected(AppendError::Fenced));
                return;
            }
            done(**r);
        });
    return true;
}

bool RoomRegistry::media_generation(const core::RoomId& room, MediaStep step,
                                    StoreCallback<std::optional<std::uint64_t>> done) {
    const auto generation = owned(room);
    if (!generation) {
        return false;
    }
    store_.media_generation(room, *generation, step,
                            [this, room, generation = *generation, done = std::move(done)](
                                StoreResult<std::optional<std::uint64_t>> r) mutable noexcept {
                                if (r && !*r) {
                                    fenced(room, generation, OwnerWrite::MediaGeneration);
                                }
                                done(r);
                            });
    return true;
}

void RoomRegistry::fenced(const core::RoomId& room, std::uint64_t generation, OwnerWrite write) {
    ++counters_.fenced_writes;
    if (owned(room) == generation) {
        disown(room);
    }
    const auto cached = cache_.find(room);
    if (cached != cache_.end() && cached->second.node == self_ &&
        cached->second.generation <= generation) {
        cache_.erase(cached);
    }
    observer_.on_fenced_out(room, generation, write);
}

void RoomRegistry::set_interest(const core::RoomId& room, bool interested) {
    if (interested) {
        interest_.insert(room);
        return;
    }
    interest_.erase(room);
    if (!owned_.contains(room)) {
        cache_.erase(room);
    }
}

void RoomRegistry::tick() {
    beat();
    sweep();
}

void RoomRegistry::beat() {
    if (beating_) {
        return;
    }
    beating_ = true;
    std::vector<OwnedRoom> rooms;
    rooms.reserve(owned_.size());
    for (const auto& [room, generation] : owned_) {
        rooms.push_back({.room = room, .generation = generation, .last_seq = 0});
    }
    // The rooms as sent: one taken after this point is not in the answer, and must not be
    // judged by it.
    std::vector<OwnedRoom> sent = rooms;
    store_.heartbeat(
        self_, incarnation_, std::move(rooms),
        [this, sent = std::move(sent)](StoreResult<std::vector<core::RoomId>> renewed) noexcept {
            beating_ = false;
            if (!renewed) {
                return;
            }
            last_beat_ = clock_.now();
            for (const OwnedRoom& r : sent) {
                // A room taken again under a newer generation since is not fenced.
                if (owned(r.room) == r.generation &&
                    std::ranges::find(*renewed, r.room) == renewed->end()) {
                    fenced(r.room, r.generation, OwnerWrite::Heartbeat);
                }
            }
        });
}

void RoomRegistry::sweep() {
    if (sweeping_) {
        return;
    }
    std::vector<core::RoomId> candidates;
    // A room being looked up is left to the lookup, which may take it too: two claims of one
    // room in flight at once would each raise the generation, the second fencing the first.
    for (const core::RoomId& room : interest_) {
        if (!owned_.contains(room) && !resolving_.contains(room)) {
            candidates.push_back(room);
        }
    }
    if (candidates.empty()) {
        return;
    }
    sweeping_ = true;
    store_.claim_stale(std::move(candidates), self_,
                       [this](StoreResult<std::vector<OwnedRoom>> claimed) noexcept {
                           sweeping_ = false;
                           if (!claimed) {
                               return;
                           }
                           for (const OwnedRoom& r : *claimed) {
                               acquired(r.room, r.generation, r.last_seq);
                           }
                       });
}

void RoomRegistry::release_all(StoreCallback<void> done) {
    std::vector<OwnedRoom> rooms;
    rooms.reserve(owned_.size());
    for (const auto& [room, generation] : owned_) {
        rooms.push_back({.room = room, .generation = generation, .last_seq = 0});
        cache_.erase(room);
    }
    owned_.clear();
    taken_at_.clear();
    store_.release(self_, std::move(rooms), std::move(done));
}

void RoomRegistry::release(const core::RoomId& room) {
    const auto it = owned_.find(room);
    if (it == owned_.end()) {
        return;
    }
    std::vector<OwnedRoom> rooms{{.room = room, .generation = it->second, .last_seq = 0}};
    disown(room);
    cache_.erase(room);
    // Failing leaves the room to go stale instead, which costs a claimant kOwnerStaleAfter.
    store_.release(self_, std::move(rooms), [](const StoreResult<void>&) noexcept {});
}

std::vector<core::RoomId> RoomRegistry::owned_rooms() const {
    std::vector<core::RoomId> rooms;
    rooms.reserve(owned_.size());
    for (const auto& [room, generation] : owned_) {
        rooms.push_back(room);
    }
    return rooms;
}

bool RoomRegistry::healthy() const noexcept {
    return last_beat_ && clock_.now() - *last_beat_ < kOwnerStaleAfter;
}

void RoomRegistry::on_owner_changed(const core::RoomId& room, const Ownership& owner) noexcept {
    if (!interest_.contains(room) && !cache_.contains(room) && !owned_.contains(room)) {
        return;
    }
    // This node's own claims are reported by their answers, which also record the ownership.
    if (learn(room, owner) && owner.node != self_) {
        observer_.on_owner_changed(room, owner);
    }
}

void RoomRegistry::on_resync() noexcept {
    std::erase_if(cache_, [this](const auto& entry) { return entry.second.node != self_; });
    for (const core::RoomId& room : interest_) {
        if (!owned_.contains(room)) {
            resolve(room, [](const StoreResult<Ownership>&) noexcept {});
        }
    }
}

} // namespace rt
