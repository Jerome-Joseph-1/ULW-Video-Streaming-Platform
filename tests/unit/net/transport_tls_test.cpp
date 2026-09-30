#include "net/offload_pool.hpp"
#include "net/reactor_factory.hpp"
#include "net/transport.hpp"

#include "support/fake_clock.hpp"
#include "support/reactor_harness.hpp"
#include "support/tls_pki.hpp"

#include <sys/socket.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
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

struct Reloads final : net::IReloadHandler {
    std::vector<std::expected<void, std::string>> results;
    void on_reloaded(const std::expected<void, std::string>& result) noexcept override {
        results.push_back(result);
    }
};

std::string file_text(const std::string& path) {
    const std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

class TlsTransportTest : public ::testing::TestWithParam<ReactorKind> {
protected:
    void SetUp() override {
        auto r = net::make_reactor(GetParam(), clock, 4096);
        ASSERT_TRUE(r) << std::strerror(r.error());
        reactor = std::move(*r);
        auto p = net::OffloadPool::create(*reactor, 1);
        ASSERT_TRUE(p) << std::strerror(p.error());
        pool = std::move(*p);
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

    std::expected<void, std::string> reload() {
        const std::size_t before = reloads.results.size();
        transports->reload(*pool, reloads);
        if (!pump_until(*reactor, [&] { return reloads.results.size() > before; })) {
            return std::unexpected("reload never finished");
        }
        return reloads.results.back();
    }

    // Connects with `ctx`, offering `session` when there is one, and returns once the client
    // holds a ticket. The connection and its handler stay until the test ends.
    TlsPeer* connect_for_ticket(SSL_CTX* ctx, SSL_SESSION* session) {
        auto h = std::make_unique<Held>();
        h->peer = connect(h->upper, ctx);
        if (session != nullptr) {
            EXPECT_EQ(SSL_set_session(h->peer->ssl(), session), 1);
        }
        EXPECT_TRUE(handshake(*h->peer));
        std::vector<std::byte> none;
        EXPECT_TRUE(pump_until(*reactor, [&] {
            h->peer->read(none);
            return SSL_SESSION_is_resumable(SSL_get0_session(h->peer->ssl())) == 1;
        }));
        held.push_back(std::move(h));
        return held.back()->peer.get();
    }

    // The client's session, ticket included, for as long as the test keeps it.
    static ulw::test::SslSessionPtr ticket_of(TlsPeer& peer) {
        return ulw::test::SslSessionPtr{SSL_get1_session(peer.ssl())};
    }

    // Moves the clock on and lets the loop run whatever came due.
    void idle_for(Millis span) {
        clock.advance(span);
        pump_pending(*reactor);
    }

    void TearDown() override {
        for (auto& t : owned) {
            t->begin_close();
        }
        pump_pending(*reactor);
    }

    ulw::test::FakeClock clock;
    std::unique_ptr<net::IReactor> reactor;
    Reloads reloads;
    std::unique_ptr<net::ITransportFactory> transports;
    ulw::test::SslCtxPtr client_ctx;
    // Connections kept by connect_for_ticket; their handlers outlive the transports below.
    struct Held {
        Upper upper;
        std::unique_ptr<TlsPeer> peer;
    };
    std::vector<std::unique_ptr<Held>> held;
    std::vector<std::unique_ptr<net::ITransport>> owned;
    // Destroyed first: a reload it runs points at the transports and at `reloads`.
    std::unique_ptr<net::OffloadPool> pool;
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

TEST_P(TlsTransportTest, ResumingFromTheLoopKeepsParkedBytesWithinOneReceive) {
    // What the gateway does: pause on every record the store cannot take, resume later from a
    // store callback, never from inside on_data.
    const auto tls13 = TestPki::shared().client_context(TLS1_3_VERSION);
    Upper server;
    server.hook = [](Upper& u, net::BorrowedBytes) { u.transport->stop_receiving(); };
    auto client = connect(server, tls13.get());
    ASSERT_TRUE(handshake(*client));
    ASSERT_TRUE(pump_until(*reactor, [&] { return transports->handshakes_in_flight() == 0; }));

    const auto data = pattern(16 * kMiB, 9);
    client->write(data);
    // Full records only, each 16 KiB of plaintext plus a 5-byte header, the inner content type
    // and a 16-byte AEAD tag.
    constexpr std::size_t kRecordOnWire = kRecord + 5 + 1 + 16;
    // One reactor receive is at most 64 KiB, on both reactors.
    constexpr std::size_t kOneReceive = 64 * kKiB;
    std::size_t held_most = 0;
    for (int cycle = 0; cycle < 200; ++cycle) {
        client->io();
        pump_pending(*reactor);
        ASSERT_EQ(server.received.size() % kRecord, 0U);
        // Asked of the transport rather than inferred from the socket: FIONREAD on a Unix
        // socket pair counts differently from one kernel to the next.
        held_most = std::max(held_most, server.transport->pending_receive_bytes());
        server.transport->start_receiving();
    }
    EXPECT_GT(server.data_calls, 100);
    EXPECT_LE(held_most, kOneReceive + kRecordOnWire);

    server.hook = nullptr;
    server.transport->start_receiving();
    ASSERT_TRUE(pump_until(
        *reactor,
        [&] {
            client->io();
            return server.received.size() == data.size();
        },
        std::chrono::seconds(60)));
    EXPECT_TRUE(server.received == data);
}

TEST_P(TlsTransportTest, AResumeAfterTheLastParkedRecordReadsTheSocketAgain) {
    const auto tls13 = TestPki::shared().client_context(TLS1_3_VERSION);
    Upper server;
    server.hook = [](Upper& u, net::BorrowedBytes) { u.transport->stop_receiving(); };
    auto client = connect(server, tls13.get());
    ASSERT_TRUE(handshake(*client));
    // Two whole records in one receive: the pause after the first parks the second, and
    // reading it leaves nothing buffered, not even part of a record.
    client->write(pattern(2 * kRecord, 1));
    ASSERT_TRUE(pump_until(*reactor, [&] {
        client->io();
        return server.data_calls == 1;
    }));
    server.transport->start_receiving();
    ASSERT_TRUE(pump_until(*reactor, [&] { return server.data_calls == 2; }));
    server.transport->start_receiving();
    pump_pending(*reactor);

    client->write(pattern(kRecord, 2));
    EXPECT_TRUE(pump_until(*reactor, [&] {
        client->io();
        return server.data_calls == 3;
    }));
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

TEST_P(TlsTransportTest, FailedHandshakesAreCountedAndLoggedAtMostOnceASecond) {
    const auto fail_one = [&] {
        Upper server;
        auto [server_fd, client_fd] = ulw::test::unix_pair();
        auto t = transports->attach(std::move(server_fd), server);
        ASSERT_TRUE(t);
        server.transport = t->get();
        server.transport->start_receiving();
        const std::string_view http = "GET / HTTP/1.1\r\n\r\n";
        ASSERT_EQ(ulw::test::write_some(client_fd.get(), std::as_bytes(std::span(http))),
                  http.size());
        ASSERT_TRUE(pump_until(*reactor, [&] { return server.error.has_value(); }));
        server.transport->begin_close();
        pump_pending(*reactor);
    };
    const auto lines = [](const std::string& text) { return std::ranges::count(text, '\n'); };

    ::testing::internal::CaptureStderr();
    for (int i = 0; i < 20; ++i) {
        fail_one();
    }
    // A peer that leaves half way is a failed handshake too, and is not logged.
    {
        Upper server;
        auto client = connect(server);
        client->handshake_step();
        client->fin();
        ASSERT_TRUE(pump_until(*reactor, [&] { return server.eof; }));
    }
    const std::string burst = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(transports->handshake_failures(), 21U);
    EXPECT_EQ(lines(burst), 1) << burst;

    clock.advance(Millis{1'000});
    ::testing::internal::CaptureStderr();
    fail_one();
    const std::string next = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(transports->handshake_failures(), 22U);
    EXPECT_EQ(lines(next), 1) << next;
    EXPECT_NE(next.find("(19 more not logged)"), std::string::npos) << next;
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
    ASSERT_TRUE(reload());
    Upper two;
    auto b = connect(two);
    ASSERT_TRUE(handshake(*b));
    EXPECT_EQ(ulw::test::peer_common_name(b->ssl()), "second.localhost");

    std::filesystem::resize_file(files.private_key, 10);
    const auto refused = reload();
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find(files.private_key), std::string::npos);
    Upper three;
    auto c = connect(three);
    ASSERT_TRUE(handshake(*c));
    EXPECT_EQ(ulw::test::peer_common_name(c->ssl()), "second.localhost");
}

TEST_P(TlsTransportTest, AReloadReadsItsFilesOffTheLoop) {
    const std::string stem =
        GetParam() == ReactorKind::IoUring ? "offloop-io-uring" : "offloop-epoll";
    auto files = TestPki::shared().issue(stem, "first.localhost");
    auto made = net::make_tls_transports(*reactor, files);
    ASSERT_TRUE(made) << made.error();
    transports = std::move(*made);
    static_cast<void>(TestPki::shared().issue(stem, "second.localhost"));
    // The renewed chain comes through a FIFO, whose open blocks until someone writes it: a
    // reload that read it on the loop would stop the loop until then.
    const std::string chain = file_text(files.certificate_chain);
    std::filesystem::remove(files.certificate_chain);
    ASSERT_EQ(::mkfifo(files.certificate_chain.c_str(), 0600), 0);
    std::promise<void> go;
    std::jthread writer([&, released = go.get_future()] {
        // A loop stalled by the reload never releases the writer; the deadline ends the test.
        released.wait_for(std::chrono::seconds(10));
        const os::UniqueFd out{::open(files.certificate_chain.c_str(), O_WRONLY | O_CLOEXEC)};
        ASSERT_TRUE(out);
        ASSERT_EQ(::write(out.get(), chain.data(), chain.size()),
                  static_cast<ssize_t>(chain.size()));
    });

    transports->reload(*pool, reloads);
    Upper one;
    auto a = connect(one);
    ASSERT_TRUE(handshake(*a));
    EXPECT_EQ(ulw::test::peer_common_name(a->ssl()), "first.localhost");
    EXPECT_TRUE(reloads.results.empty());

    go.set_value();
    ASSERT_TRUE(pump_until(*reactor, [&] { return !reloads.results.empty(); }));
    ASSERT_TRUE(reloads.results.front()) << reloads.results.front().error();
    writer.join();
    std::filesystem::remove(files.certificate_chain);
    Upper two;
    auto b = connect(two);
    ASSERT_TRUE(handshake(*b));
    EXPECT_EQ(ulw::test::peer_common_name(b->ssl()), "second.localhost");
}

TEST_P(TlsTransportTest, ATicketIssuedBeforeAReloadResumesAfterIt) {
    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        SCOPED_TRACE(version);
        const auto ctx = TestPki::shared().client_context(version);
        Upper first;
        auto a = connect(first, ctx.get());
        ASSERT_TRUE(handshake(*a));
        std::vector<std::byte> none;
        ASSERT_TRUE(pump_until(*reactor, [&] {
            a->read(none);
            return SSL_SESSION_is_resumable(SSL_get0_session(a->ssl())) == 1;
        }));

        ASSERT_TRUE(reload());
        Upper second;
        auto b = connect(second, ctx.get());
        ASSERT_EQ(SSL_set_session(b->ssl(), SSL_get0_session(a->ssl())), 1);
        ASSERT_TRUE(handshake(*b));
        EXPECT_EQ(SSL_session_reused(b->ssl()), 1);
        first.transport->begin_close();
        second.transport->begin_close();
    }
}

// A ticket holds its session's master secret, so a key that sealed tickets for good would open
// every resumed session ever made under it. The sealing key is replaced every 12 hours and the
// one before it opens tickets for 12 more; past that a ticket buys a full handshake.
TEST_P(TlsTransportTest, TicketKeysRotateAndATicketStopsResumingOnceItsKeyIsGone) {
    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        SCOPED_TRACE(version);
        const auto ctx = TestPki::shared().client_context(version);
        const auto ticket = ticket_of(*connect_for_ticket(ctx.get(), nullptr));

        // One interval on, a newer key seals, and the old one still opens.
        clock.advance(std::chrono::hours(12) + std::chrono::seconds(1));
        TlsPeer* later = connect_for_ticket(ctx.get(), ticket.get());
        EXPECT_EQ(SSL_session_reused(later->ssl()), 1);
        const auto fresh = ticket_of(*later);

        // Another interval on, the key that sealed the first ticket is gone; the ticket the
        // second connection was given, sealed with the key after it, still resumes.
        clock.advance(std::chrono::hours(12) + std::chrono::seconds(1));
        TlsPeer* refused = connect_for_ticket(ctx.get(), ticket.get());
        EXPECT_EQ(SSL_session_reused(refused->ssl()), 0);
        TlsPeer* renewed = connect_for_ticket(ctx.get(), fresh.get());
        EXPECT_EQ(SSL_session_reused(renewed->ssl()), 1);
        for (const auto& h : held) {
            h->upper.transport->begin_close();
        }
        pump_pending(*reactor);
    }
}

// The keys are replaced on schedule whether or not a handshake comes along to do it, so a key is
// wiped a day after it was made on a server nobody reached in between. Were it replaced only by
// the next handshake, a server idle for 20 hours would make its second key then, and a ticket
// sealed with it would still resume 17 hours later.
TEST_P(TlsTransportTest, TicketKeysRotateOnScheduleOnAnIdleServer) {
    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        SCOPED_TRACE(version);
        const auto ctx = TestPki::shared().client_context(version);
        static_cast<void>(connect_for_ticket(ctx.get(), nullptr));

        // Nobody connects until hour 20; the second key was made at hour 12.
        idle_for(std::chrono::hours(12) + std::chrono::seconds(1));
        idle_for(std::chrono::hours(8));
        const auto sealed = ticket_of(*connect_for_ticket(ctx.get(), nullptr));

        // Hour 36 and a little: the key made at hour 12 has had its two intervals.
        idle_for(std::chrono::hours(4) + std::chrono::seconds(1));
        idle_for(std::chrono::hours(12) + std::chrono::seconds(1));
        TlsPeer* late = connect_for_ticket(ctx.get(), sealed.get());
        EXPECT_EQ(SSL_session_reused(late->ssl()), 0);
        for (const auto& h : held) {
            h->upper.transport->begin_close();
        }
        pump_pending(*reactor);
    }
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
