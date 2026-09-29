#include "os/unique_fd.hpp"

#include "ingest.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <gtest/gtest.h>
#include <stop_token>
#include <unistd.h>

namespace {

// Connects to 127.0.0.1:`port`; the descriptor is empty and errno set when it is refused.
os::UniqueFd connect_to(std::uint16_t port) {
    os::UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own cast.
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        return os::UniqueFd();
    }
    return socket;
}

TEST(IngestListener, BindsAnEphemeralPortAndReportsTheOneItGot) {
    const auto listener = live::IngestListener::bind("127.0.0.1", 0);
    ASSERT_TRUE(listener) << listener.error();
    EXPECT_NE(listener->port(), 0);
}

TEST(IngestListener, HandsOverThePublishersConnectionAsAReadableDescriptor) {
    auto listener = live::IngestListener::bind("127.0.0.1", 0);
    ASSERT_TRUE(listener);
    const os::UniqueFd publisher = connect_to(listener->port());
    ASSERT_TRUE(publisher);
    const auto connection = listener->accept({});
    ASSERT_TRUE(connection);
    ASSERT_TRUE(*connection);
    constexpr char kByte = 'x';
    ASSERT_EQ(::write(publisher.get(), &kByte, 1), 1);
    char received = 0;
    ASSERT_EQ(::read(connection->get(), &received, 1), 1);
    EXPECT_EQ(received, kByte);
}

TEST(IngestListener, RefusesASecondPublisherOnceTheFirstIsAccepted) {
    auto listener = live::IngestListener::bind("127.0.0.1", 0);
    ASSERT_TRUE(listener);
    const std::uint16_t port = listener->port();
    const os::UniqueFd first = connect_to(port);
    ASSERT_TRUE(first);
    const auto connection = listener->accept({});
    ASSERT_TRUE(connection && *connection);
    errno = 0;
    EXPECT_FALSE(connect_to(port));
    EXPECT_EQ(errno, ECONNREFUSED);
}

TEST(IngestListener, ReturnsNoConnectionWhenStoppedFirst) {
    auto listener = live::IngestListener::bind("127.0.0.1", 0);
    ASSERT_TRUE(listener);
    std::stop_source stop;
    stop.request_stop();
    const auto connection = listener->accept(stop.get_token());
    ASSERT_TRUE(connection);
    EXPECT_FALSE(*connection);
}

TEST(IngestListener, RefusesAHostNameRatherThanResolvingIt) {
    const auto listener = live::IngestListener::bind("localhost", 0);
    ASSERT_FALSE(listener);
    EXPECT_NE(listener.error().find("localhost"), std::string::npos);
}

TEST(IngestListener, RefusesAPortAnotherListenerHolds) {
    const auto first = live::IngestListener::bind("127.0.0.1", 0);
    ASSERT_TRUE(first);
    EXPECT_FALSE(live::IngestListener::bind("127.0.0.1", first->port()));
}

TEST(IngestListener, ListensOnAnIpv6AddressToo) {
    const auto listener = live::IngestListener::bind("::1", 0);
    if (!listener) {
        GTEST_SKIP() << "no IPv6 loopback on this host: " << listener.error();
    }
    EXPECT_NE(listener->port(), 0);
}

} // namespace
