#include "codec/ws/decoder.hpp"
#include "codec/ws/encoder.hpp"
#include "codec/ws/frame.hpp"
#include "net/reactor_factory.hpp"
#include "net/socket.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_random.hpp"
#include "support/reactor_harness.hpp"
#include "wire.hpp"
#include "ws_echo_server.hpp"

#include <sys/socket.h>

#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

using codec::ws::CloseCode;
using codec::ws::Opcode;
using net::ReactorKind;
using ulw::test::Bytes;
using ulw::test::message;
using ulw::test::pump_until;
using ulw::test::read_some;
using ulw::test::text;
using ulw::test::write_some;

constexpr std::string_view kUpgrade = "GET /echo HTTP/1.1\r\n"
                                      "Host: localhost\r\n"
                                      "Upgrade: websocket\r\n"
                                      "Connection: Upgrade\r\n"
                                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                      "Sec-WebSocket-Version: 13\r\n"
                                      "\r\n";

// The server's frames are unmasked, which a client-side decoder would refuse; this reads them
// by hand, as much as the tests need: short payloads, no fragmentation.
struct ServerFrame {
    std::byte first{};
    Bytes payload;
};

std::optional<ServerFrame> take_frame(Bytes& in) {
    if (in.size() < 2 || in.size() < 2 + std::to_integer<std::size_t>(in[1])) {
        return std::nullopt;
    }
    const auto n = std::to_integer<std::size_t>(in[1]);
    ServerFrame f{.first = in[0],
                  .payload =
                      Bytes(in.begin() + 2, in.begin() + 2 + static_cast<std::ptrdiff_t>(n))};
    in.erase(in.begin(), in.begin() + 2 + static_cast<std::ptrdiff_t>(n));
    return f;
}

class WsEchoServerTest : public ::testing::TestWithParam<ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 4096);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        server = std::make_unique<ulw::test::WsEchoServer>(*reactor, ulw::test::WsEchoOptions{});
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        ASSERT_TRUE(listener);
        port = *net::local_port(listener->get());
        ASSERT_TRUE(reactor->listen(std::move(*listener), *server));
        client = ulw::test::connect_loopback(port);
        ASSERT_TRUE(client);
    }

    void TearDown() override { server.reset(); }

    template <class Pred> bool settle(Pred pred) {
        return pump_until(*reactor, [&] {
            server->reap();
            read_some(client.get(), received);
            return pred();
        });
    }

    void send(std::span<const std::byte> bytes) {
        ASSERT_EQ(write_some(client.get(), bytes), bytes.size());
    }

    void upgrade() {
        send(std::as_bytes(std::span{kUpgrade}));
        const std::string_view end = "\r\n\r\n";
        ASSERT_TRUE(
            settle([&] { return ulw::test::as_text(received).find(end) != std::string::npos; }));
        const std::string head = ulw::test::as_text(received);
        ASSERT_TRUE(head.starts_with("HTTP/1.1 101 Switching Protocols\r\n")) << head;
        EXPECT_NE(head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"),
                  std::string::npos);
        received.erase(received.begin(),
                       received.begin() + static_cast<std::ptrdiff_t>(head.find(end) + end.size()));
    }

    ServerFrame next_frame() {
        std::optional<ServerFrame> f;
        EXPECT_TRUE(settle([&] {
            f = take_frame(received);
            return f.has_value();
        }));
        return f.value_or(ServerFrame{});
    }

    bool peer_closed() {
        return settle([&] {
            std::byte b{};
            return ::recv(client.get(), &b, 1, MSG_DONTWAIT) == 0;
        });
    }

    Bytes client_frame(const codec::ws::Frame& frame) {
        Bytes out;
        EXPECT_TRUE(encoder.encode(frame, out));
        return out;
    }

    ulw::test::FakeClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<ulw::test::WsEchoServer> server;
    std::uint16_t port = 0;
    os::UniqueFd client;
    Bytes received;
    ulw::test::FakeRandom keys{7};
    codec::ws::ClientEncoder encoder{keys};
};

TEST_P(WsEchoServerTest, EchoesTextAndAnswersPing) {
    upgrade();
    send(client_frame(message(Opcode::Text, text("hello"))));
    send(client_frame(message(Opcode::Ping, text("p"))));

    const ServerFrame echo = next_frame();
    const ServerFrame pong = next_frame();

    EXPECT_EQ(echo.first, std::byte{0x81});
    EXPECT_EQ(echo.payload, text("hello"));
    EXPECT_EQ(pong.first, std::byte{0x8A});
    EXPECT_EQ(pong.payload, text("p"));
}

TEST_P(WsEchoServerTest, ReturnsACloseWithItsStatusThenHangsUp) {
    upgrade();
    send(client_frame(ulw::test::close_frame(CloseCode{4001}, "done")));

    const ServerFrame close = next_frame();

    EXPECT_EQ(close.first, std::byte{0x88});
    EXPECT_EQ(close.payload, ulw::test::bytes({0x0F, 0xA1}));
    ASSERT_EQ(::shutdown(client.get(), SHUT_WR), 0);
    EXPECT_TRUE(peer_closed());
}

TEST_P(WsEchoServerTest, AnswersAProtocolErrorWithItsStatus) {
    upgrade();
    send(ulw::test::raw_frame(0x81, "unmasked", /*masked=*/false));

    const ServerFrame close = next_frame();

    EXPECT_EQ(close.first, std::byte{0x88});
    EXPECT_EQ(close.payload, ulw::test::bytes({0x03, 0xEA}));
    EXPECT_TRUE(peer_closed());
}

TEST_P(WsEchoServerTest, RefusesAnUpgradeWithTheWrongVersion) {
    std::string request{kUpgrade};
    request.replace(request.find("Version: 13"), 11, "Version: 12");
    send(std::as_bytes(std::span{request}));

    EXPECT_TRUE(peer_closed());
    EXPECT_TRUE(ulw::test::as_text(received).starts_with("HTTP/1.1 426 Upgrade Required\r\n"));
}

INSTANTIATE_TEST_SUITE_P(Reactors, WsEchoServerTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
