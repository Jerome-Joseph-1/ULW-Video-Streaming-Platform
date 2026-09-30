// Ports handed to child processes: a port the kernel can also give an outgoing connection as its
// source port can be taken between the reservation and the child's listen(), and the child fails
// to start for a reason that has nothing to do with the change under test.
#include "support/reserve_port.hpp"

#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <set>

namespace {

TEST(ReservePort, NeverHandsOutAPortFromTheEphemeralRange) {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    std::ifstream range("/proc/sys/net/ipv4/ip_local_port_range");
    if (!(range >> low >> high)) {
        GTEST_SKIP() << "cannot read /proc/sys/net/ipv4/ip_local_port_range";
    }
    // A range from 21000 or below up to 65535 leaves reserve_port() nothing outside it.
    if (const auto window = ulw::test::detail::port_window(); window.first >= window.end) {
        GTEST_SKIP() << "the ephemeral range " << low << "-" << high
                     << " leaves no port outside it";
    }
    std::set<std::uint16_t> seen;
    constexpr int kReservations = 64;
    for (int i = 0; i < kReservations; ++i) {
        const std::uint16_t port = ulw::test::reserve_port();
        ASSERT_NE(port, 0);
        EXPECT_TRUE(port < low || port > high)
            << port << " is inside the ephemeral range " << low << "-" << high;
        seen.insert(port);
    }
    // Reservations made before any child binds must not collide with one another.
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kReservations));
}

} // namespace
