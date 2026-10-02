#pragma once

#include "core/ports/catalog.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <expected>

namespace core::ports {

// What one forget_unused call did.
struct UnusedRoomsScan {
    // Rooms forgotten.
    std::size_t forgotten = 0;
    // The walk reached the last room recorded at or before the cutoff: the next call starts over
    // from the oldest.
    bool finished = false;
};

// The message store's side of the upload reaper. Blocking: for the reaper's own thread, never a
// reactor's.
class IUnusedRooms {
public:
    virtual ~IUnusedRooms() = default;
    // Looks at up to `limit` rooms whose kind was recorded at or before `recorded_before`, oldest
    // first, from where the last call stopped (kept in the database, across passes), and forgets
    // those a join recorded that were never used: a direct or group chat that lists no members
    // and was never resolved on the room plane, and so holds no message. That is the record a
    // refused join left, and nothing else; a stream's live chat, which the server opens and
    // lists nobody, is never forgotten. Every such room is forgotten eventually, however old: the
    // walk goes on from the oldest once it reaches the cutoff. A join of a forgotten room
    // afterwards records it again, possibly as the other closed kind, and is answered as before.
    [[nodiscard]] virtual std::expected<UnusedRoomsScan, CatalogError>
    forget_unused(WallTime recorded_before, std::size_t limit) = 0;
};

} // namespace core::ports
