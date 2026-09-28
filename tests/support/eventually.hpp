#pragma once

#include <chrono>
#include <thread>

namespace ulw::test {

// For conditions another thread brings about: re-checks until `pred` holds or `limit` passes.
// Each check is expected to synchronise with that thread (e.g. a round trip to its loop), so
// this yields rather than sleeps.
template <class Pred>
bool eventually(Pred pred, std::chrono::milliseconds limit = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

} // namespace ulw::test
