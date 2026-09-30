#pragma once

#include <cstdint>
#include <optional>

namespace ulw::test {

// What the kernel says of a server's end of a loopback connection, found by its ports.
struct SendWindow {
    // Bytes queued that the kernel has not sent yet.
    std::uint64_t unsent = 0;
    // Bytes sent that the peer has not acknowledged.
    std::int64_t in_flight = 0;
    // Bytes the peer's advertised window has room for beyond what is in flight.
    std::int64_t room = 0;
    // Bytes sent so far, retransmissions excluded.
    std::uint64_t sent = 0;
};

// The socket of this process bound to `local` and connected to `remote`; nullopt when there is
// none, or the kernel does not report the window. Only reads its state.
[[nodiscard]] std::optional<SendWindow> send_window_of(std::uint16_t local, std::uint16_t remote);

} // namespace ulw::test
