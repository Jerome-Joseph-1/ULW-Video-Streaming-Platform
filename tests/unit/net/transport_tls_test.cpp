#include "net/reactor_factory.hpp"
#include "net/transport.hpp"

#include "support/fake_clock.hpp"
#include "support/reactor_harness.hpp"
#include "support/tls_pki.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <optional>
#include <vector>

namespace {

using core::Millis;
using net::ReactorKind;
using ulw::test::kKiB;
using ulw::test::kMiB;
using ulw::test::pattern;
using ulw::test::pump_pending;
using ulw::test::pump_until;
using ulw::test::SslPtr;
using ulw::test::TestPki;

constexpr Millis kHandshakeTimeout{5'000};
// A TLS record carries at most 16 KiB of plaintext (RFC 8446 section 5.1).
constexpr std::size_t kRecord = 16 * kKiB;

// What the protocol above the transport sees.
struct Upper final : net::IStreamHandler {
    net::ITransport* transport = nullptr;
    std::vector<std::byte> received;
    int data_calls = 0;
    std::size_t largest = 0;
    int writable_calls = 0;
    bool eof = false;
    std::optional<int> error;
    std::function<void(Upper&, net::BorrowedBytes)> hook;

    void on_data(net::BorrowedBytes bytes) noexcept override {
        ++data_calls;
        largest = std::max(largest, bytes.size());
        received.insert(received.end(), bytes.begin(), bytes.end());
        if (hook) {
            hook(*this, bytes);
        }
    }
    void on_writable() noexcept override { ++writable_calls; }
    void on_peer_eof() noexcept override { eof = true; }
    void on_error(int err) noexcept override { error = err; }
};

// An OpenSSL client on the other end of the socket, driven by hand from the test thread
// through memory BIOs, so that the test decides exactly which bytes reach the server and when.
class TlsPeer {
public:
    TlsPeer(SSL_CTX* ctx, os::UniqueFd fd) : fd_(std::move(fd)), ssl_(SSL_new(ctx)) {
        BIO* in = BIO_new(BIO_s_mem());
        BIO* out = BIO_new(BIO_s_mem());
        BIO_set_mem_eof_return(in, -1);
        SSL_set_bio(ssl_.get(), in, out);
        SSL_set_connect_state(ssl_.get());
    }

    [[nodiscard]] SSL* ssl() const { return ssl_.get(); }
    [[nodiscard]] int fd() const { return fd_.get(); }
    [[nodiscard]] std::size_t unsent() const { return outbox_.size(); }
    // At most this many bytes reach the socket per io().
    void limit_writes(std::size_t n) { max_write_ = n; }

    // Moves what the socket has into the client, and what the client wrote towards the socket
    // as far as the kernel takes it.
    void io() {
        std::vector<std::byte> got;
        ulw::test::read_some(fd_.get(), got);
        if (!got.empty()) {
            BIO_write(SSL_get_rbio(ssl_.get()), got.data(), static_cast<int>(got.size()));
        }
        std::array<std::byte, 65536> buf{};
        for (;;) {
            const int n = BIO_read(SSL_get_wbio(ssl_.get()), buf.data(), buf.size());
            if (n <= 0) {
                break;
            }
            outbox_.insert(outbox_.end(), buf.begin(), buf.begin() + n);
        }
        const std::size_t sent = ulw::test::write_some(
            fd_.get(), std::span(outbox_).first(std::min(outbox_.size(), max_write_)));
        outbox_.erase(outbox_.begin(), outbox_.begin() + static_cast<std::ptrdiff_t>(sent));
    }

    bool handshake_step() {
        io();
        ERR_clear_error();
        const int r = SSL_do_handshake(ssl_.get());
        io();
        return r == 1;
    }

    void write(std::span<const std::byte> bytes) {
        std::size_t n = 0;
        ASSERT_EQ(SSL_write_ex(ssl_.get(), bytes.data(), bytes.size(), &n), 1);
        ASSERT_EQ(n, bytes.size());
        io();
    }

    // Reads whatever plaintext is available; returns the SSL error that ended the read.
    int read(std::vector<std::byte>& into) {
        io();
        std::array<std::byte, 65536> buf{};
        for (;;) {
            std::size_t n = 0;
            ERR_clear_error();
            if (SSL_read_ex(ssl_.get(), buf.data(), buf.size(), &n) != 1) {
                return SSL_get_error(ssl_.get(), 0);
            }
            into.insert(into.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }

    void close_notify() {
        SSL_shutdown(ssl_.get());
        io();
    }

    void fin() { ::shutdown(fd_.get(), SHUT_WR); }

private:
    os::UniqueFd fd_;
    SslPtr ssl_;
    std::vector<std::byte> outbox_;
    std::size_t max_write_ = SIZE_MAX;
};

class TlsTransportTest : public ::testing::TestWithParam<ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 4096);
        ASSERT_TRUE(r) << std::strerror(r.error());
        reactor = std::move(*r);
        auto t = net::make_tls_transports(*reactor, TestPki::shared().server());
        ASSERT_TRUE(t) << t.error();
        transports = std::move(*t);
        client_ctx = TestPki::shared().client_context();
    }

    // Attaches the server end of a socket pair to `upper` and returns a client on the other.
    std::unique_ptr<TlsPeer> connect(Upper& upper, SSL_CTX* ctx = nullptr) {
        auto [server, client] = ulw::test::unix_pair();
        auto t = transports->attach(std::move(server), upper);
        EXPECT_TRUE(t);
        if (!t) {
            return nullptr;
        }
        owned.push_back(std::move(*t));
        upper.transport = owned.back().get();
        upper.transport->start_receiving();
        return std::make_unique<TlsPeer>(ctx == nullptr ? client_ctx.get() : ctx,
                                         std::move(client));
    }

    bool handshake(TlsPeer& peer) {
        return pump_until(*reactor, [&] { return peer.handshake_step(); });
    }

    void TearDown() override {
        for (auto& t : owned) {
            t->begin_close();
        }
        pump_pending(*reactor);
    }

    ulw::test::FakeClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<net::ITransportFactory> transports;
    ulw::test::SslCtxPtr client_ctx;
    std::vector<std::unique_ptr<net::ITransport>> owned;
};

TEST_P(TlsTransportTest, HandshakeThenPlaintextBothWays) {
    Upper server;
    server.hook = [](Upper& u, net::BorrowedBytes b) { u.transport->send(b); };
    auto client = connect(server);
    EXPECT_EQ(transports->handshakes_in_flight(), 1U);
    ASSERT_TRUE(handshake(*client));
    EXPECT_EQ(ulw::test::peer_common_name(client->ssl()), "localhost");
    ASSERT_TRUE(pump_until(*reactor, [&] { return transports->handshakes_in_flight() == 0; }));

    const std::string_view msg = "GET /api/v1/healthz HTTP/1.1\r\n\r\n";
    client->write(std::as_bytes(std::span(msg)));
    std::vector<std::byte> echoed;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        client->read(echoed);
        return echoed.size() >= msg.size();
    }));
    EXPECT_EQ(ulw::test::as_text(echoed), msg);
    EXPECT_EQ(ulw::test::as_text(server.received), msg);
    EXPECT_FALSE(server.error);
}

TEST_P(TlsTransportTest, AHandshakeFedOneByteAtATimeCompletes) {
    Upper server;
    auto client = connect(server);
    client->limit_writes(1);
    int steps = 0;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        ++steps;
        return client->handshake_step() && client->unsent() == 0 &&
               transports->handshakes_in_flight() == 0;
    }));
    // A ClientHello and a Finished are hundreds of bytes: as many separate deliveries.
    EXPECT_GT(steps, 100);
    EXPECT_FALSE(server.error);
}

TEST_P(TlsTransportTest, OneLargeSendArrivesWholeThroughPartialWrites) {
    Upper server;
    const auto data = pattern((3 * kMiB) + 17);
    server.hook = [&](Upper& u, net::BorrowedBytes) {
        u.transport->send(data);
        // More than the socket takes at once: the rest waits in the reactor's queue.
        EXPECT_GT(u.transport->pending_send_bytes(), 0U);
    };
    auto client = connect(server);
    ASSERT_TRUE(handshake(*client));
    const std::string_view go = "go";
    client->write(std::as_bytes(std::span(go)));
    std::vector<std::byte> got;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        client->read(got);
        return got.size() >= data.size();
    }));
    EXPECT_TRUE(got == data);
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.writable_calls > 0; }));
    EXPECT_EQ(server.transport->pending_send_bytes(), 0U);
}

TEST_P(TlsTransportTest, StopReceivingIsExactAndTheBacklogStaysInTheKernel) {
    Upper server;
    server.hook = [](Upper& u, net::BorrowedBytes) { u.transport->stop_receiving(); };
    auto client = connect(server);
    ASSERT_TRUE(handshake(*client));
    const auto data = pattern(8 * kMiB, 3);
    client->write(data);
    ASSERT_TRUE(pump_until(*reactor, [&] {
        client->io();
        return server.data_calls > 0;
    }));
    for (int i = 0; i < 20; ++i) {
        client->io();
        pump_pending(*reactor);
    }
    EXPECT_EQ(server.data_calls, 1);
    EXPECT_LE(server.largest, kRecord);
    // The server stopped reading: most of the 8 MiB is still on the client's side, because
    // the transport did not keep draining the socket into its own buffers.
    EXPECT_GT(client->unsent(), 4 * kMiB);

    server.hook = [](Upper& u, net::BorrowedBytes) {
        // Pause after every record, resume from the loop: the worst case for parked bytes.
        u.transport->stop_receiving();
        u.transport->start_receiving();
    };
    server.transport->start_receiving();
    ASSERT_TRUE(pump_until(
        *reactor,
        [&] {
            client->io();
            return server.received.size() == data.size();
        },
        std::chrono::seconds(60)));
    EXPECT_TRUE(server.received == data);
    EXPECT_LE(server.largest, kRecord);
}

TEST_P(TlsTransportTest, RecordsParkedBehindAPauseArriveBeforeTheEndOfStream) {
    Upper server;
    server.hook = [](Upper& u, net::BorrowedBytes) { u.transport->stop_receiving(); };
    auto client = connect(server);
    ASSERT_TRUE(handshake(*client));
    const auto data = pattern(100 * kKiB, 5);
    client->write(data);
    client->close_notify();
    ASSERT_TRUE(pump_until(*reactor, [&] {
        client->io();
        return client->unsent() == 0 && server.data_calls > 0;
    }));
    client->fin();
    pump_pending(*reactor);
    EXPECT_FALSE(server.eof);

    server.hook = nullptr;
    server.transport->start_receiving();
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.eof; }));
    EXPECT_TRUE(server.received == data);
    EXPECT_FALSE(server.error);
}

TEST_P(TlsTransportTest, ShutdownWriteSendsCloseNotifyThenFin) {
    Upper server;
    server.hook = [](Upper& u, net::BorrowedBytes) {
        const std::string_view bye = "bye";
        u.transport->send(std::as_bytes(std::span(bye)));
        u.transport->shutdown_write();
    };
    auto client = connect(server);
    ASSERT_TRUE(handshake(*client));
    const std::string_view hi = "hi";
    client->write(std::as_bytes(std::span(hi)));
    std::vector<std::byte> got;
    int last = SSL_ERROR_NONE;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        last = client->read(got);
        return last == SSL_ERROR_ZERO_RETURN;
    }));
    EXPECT_EQ(ulw::test::as_text(got), "bye");
    EXPECT_NE(SSL_get_shutdown(client->ssl()) & SSL_RECEIVED_SHUTDOWN, 0);
    char b = 0;
    ASSERT_TRUE(
        pump_until(*reactor, [&] { return ::recv(client->fd(), &b, 1, MSG_DONTWAIT) == 0; }));

    // The read side stays open: the client can still be heard after the server's FIN.
    server.hook = nullptr;
    client->write(std::as_bytes(std::span(hi)));
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.received.size() == 4; }));
}

TEST_P(TlsTransportTest, ACloseNotifyFromThePeerIsTheEndOfStream) {
    Upper server;
    auto client = connect(server);
    ASSERT_TRUE(handshake(*client));
    client->close_notify();
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.eof; }));
    EXPECT_FALSE(server.error);
}

TEST_P(TlsTransportTest, AFinWithoutCloseNotifyIsTheEndOfStreamToo) {
    Upper server;
    auto client = connect(server);
    ASSERT_TRUE(handshake(*client));
    const std::string_view partial = "PATCH /api/v1/uploads/x HTTP/1.1\r\n";
    client->write(std::as_bytes(std::span(partial)));
    client->fin();
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.eof; }));
    EXPECT_EQ(ulw::test::as_text(server.received), partial);
    EXPECT_FALSE(server.error);
}

TEST_P(TlsTransportTest, APeerSendingGarbageFailsTheHandshake) {
    Upper server;
    auto [server_fd, client_fd] = ulw::test::unix_pair();
    auto t = transports->attach(std::move(server_fd), server);
    ASSERT_TRUE(t);
    server.transport = t->get();
    server.transport->start_receiving();
    // Plain HTTP sent to a TLS port.
    const std::string_view http = "GET /api/v1/healthz HTTP/1.1\r\nHost: t\r\n\r\n";
    ASSERT_EQ(ulw::test::write_some(client_fd.get(), std::as_bytes(std::span(http))), http.size());
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.error.has_value(); }));
    EXPECT_EQ(*server.error, EPROTO);
    EXPECT_TRUE(server.received.empty());
    EXPECT_EQ(transports->handshakes_in_flight(), 0U);
    server.transport->begin_close();
    pump_pending(*reactor);
}

TEST_P(TlsTransportTest, GarbageAfterTheHandshakeFailsTheConnection) {
    Upper server;
    auto client = connect(server);
    ASSERT_TRUE(handshake(*client));
    // An application data record header with a body that authenticates as nothing.
    std::vector<std::byte> record = {std::byte{0x17}, std::byte{0x03}, std::byte{0x03},
                                     std::byte{0x00}, std::byte{0x20}};
    record.resize(record.size() + 0x20, std::byte{0x5a});
    ASSERT_EQ(ulw::test::write_some(client->fd(), record), record.size());
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.error.has_value(); }));
    EXPECT_EQ(*server.error, EPROTO);
    EXPECT_TRUE(server.received.empty());
}

TEST_P(TlsTransportTest, AStalledHandshakeTimesOutAfterFiveSeconds) {
    Upper server;
    auto client = connect(server);
    // A ClientHello and then nothing.
    client->handshake_step();
    pump_pending(*reactor);
    EXPECT_EQ(transports->handshakes_in_flight(), 1U);
    clock.advance(kHandshakeTimeout - Millis{1});
    pump_pending(*reactor);
    EXPECT_FALSE(server.error);
    // Timers fire at most one 100 ms tick late.
    clock.advance(Millis{101});
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.error.has_value(); }));
    EXPECT_EQ(*server.error, ETIMEDOUT);
    EXPECT_EQ(transports->handshakes_in_flight(), 0U);
}

TEST_P(TlsTransportTest, ACompletedHandshakeIsNotTimedOut) {
    Upper server;
    auto client = connect(server);
    ASSERT_TRUE(handshake(*client));
    ASSERT_TRUE(pump_until(*reactor, [&] { return transports->handshakes_in_flight() == 0; }));
    clock.advance(kHandshakeTimeout * 2);
    pump_pending(*reactor);
    EXPECT_FALSE(server.error);
}

TEST_P(TlsTransportTest, RenegotiationIsRefused) {
    const auto tls12 = TestPki::shared().client_context(TLS1_2_VERSION);
    Upper server;
    auto client = connect(server, tls12.get());
    ASSERT_TRUE(handshake(*client));
    ASSERT_EQ(SSL_version(client->ssl()), TLS1_2_VERSION);
    ASSERT_EQ(SSL_renegotiate(client->ssl()), 1);
    int reason = 0;
    ASSERT_TRUE(pump_until(*reactor, [&] {
        client->io();
        ERR_clear_error();
        if (SSL_do_handshake(client->ssl()) == 1) {
            return true;
        }
        client->io();
        reason = ERR_GET_REASON(ERR_peek_error());
        return reason != 0;
    }));
    EXPECT_EQ(reason, SSL_R_NO_RENEGOTIATION);
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.error.has_value(); }));
    EXPECT_TRUE(server.received.empty());
}

TEST_P(TlsTransportTest, ASecondConnectionResumesFromItsTicket) {
    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        SCOPED_TRACE(version);
        const auto ctx = TestPki::shared().client_context(version);
        Upper first;
        auto a = connect(first, ctx.get());
        ASSERT_TRUE(handshake(*a));
        // TLS 1.3 tickets follow the handshake; reading picks them up.
        std::vector<std::byte> none;
        ASSERT_TRUE(pump_until(*reactor, [&] {
            a->read(none);
            return SSL_SESSION_is_resumable(SSL_get0_session(a->ssl())) == 1;
        }));
        SSL_SESSION* session = SSL_get0_session(a->ssl());
        // A replayable first flight is never on offer.
        EXPECT_EQ(SSL_SESSION_get_max_early_data(session), 0U);

        Upper second;
        auto b = connect(second, ctx.get());
        ASSERT_EQ(SSL_set_session(b->ssl(), session), 1);
        ASSERT_TRUE(handshake(*b));
        EXPECT_EQ(SSL_session_reused(b->ssl()), 1);
        // Both handlers go out of scope with this iteration.
        first.transport->begin_close();
        second.transport->begin_close();
    }
}

TEST_P(TlsTransportTest, AReloadServesTheNewCertificateAndAFailedOneKeepsTheOld) {
    auto files = TestPki::shared().issue("reload", "first.localhost");
    auto made = net::make_tls_transports(*reactor, files);
    ASSERT_TRUE(made) << made.error();
    transports = std::move(*made);

    Upper one;
    auto a = connect(one);
    ASSERT_TRUE(handshake(*a));
    EXPECT_EQ(ulw::test::peer_common_name(a->ssl()), "first.localhost");

    static_cast<void>(TestPki::shared().issue("reload", "second.localhost"));
    ASSERT_TRUE(transports->reload());
    Upper two;
    auto b = connect(two);
    ASSERT_TRUE(handshake(*b));
    EXPECT_EQ(ulw::test::peer_common_name(b->ssl()), "second.localhost");

    std::filesystem::resize_file(files.private_key, 10);
    const auto refused = transports->reload();
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find(files.private_key), std::string::npos);
    Upper three;
    auto c = connect(three);
    ASSERT_TRUE(handshake(*c));
    EXPECT_EQ(ulw::test::peer_common_name(c->ssl()), "second.localhost");
}

TEST_P(TlsTransportTest, AKeyThatDoesNotMatchTheCertificateIsRefused) {
    const auto one = TestPki::shared().issue("mismatch-a", "localhost");
    const auto two = TestPki::shared().issue("mismatch-b", "localhost");
    const auto made = net::make_tls_transports(
        *reactor, {.certificate_chain = one.certificate_chain, .private_key = two.private_key});
    ASSERT_FALSE(made);
    EXPECT_NE(made.error().find(two.private_key), std::string::npos);
    EXPECT_NE(made.error().find("mismatch"), std::string::npos);
    const auto missing = net::make_tls_transports(
        *reactor, {.certificate_chain = "/nonexistent/chain.pem", .private_key = two.private_key});
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().find("/nonexistent/chain.pem"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(Reactors, TlsTransportTest,
                         ::testing::Values(ReactorKind::IoUring, ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
