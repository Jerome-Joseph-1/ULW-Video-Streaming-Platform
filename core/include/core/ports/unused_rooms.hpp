#pragma once

#include "core/ports/catalog.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <expected>

namespace core::ports {

// The message store's side of the upload reaper. Blocking: for the reaper's own thread, never a
// reactor's.
class IUnusedRooms {
public:
    virtual ~IUnusedRooms() = default;
    // Forgets up to `limit` rooms whose kind was recorded at or before `recorded_before`, and
    // after `recorded_before` minus a week, that list no members and were never resolved on the
    // room plane, and so hold no message: the record a refused join left, and nothing else.
    // A join of one of them afterwards records it again and is answered as before. Returns how
    // many went.
    [[nodiscard]] virtual std::expected<std::size_t, CatalogError>
    forget_unused(WallTime recorded_before, std::size_t limit) = 0;
};

} // namespace core::ports
