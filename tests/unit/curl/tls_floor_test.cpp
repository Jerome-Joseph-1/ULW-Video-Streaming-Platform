#include "infra/curl/http.hpp"
#include "net/socket.hpp"

#include "support/tls_pki.hpp"

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <string>
#include <thread>

namespace {

using infra::curl::FailureKind;

// Takes one TLS connection and records whether it answered the ClientHello, and with which
// version. The client under test trusts only the system's CAs, not the test CA, so a handshake
// that is accepted still ends at the certificate check: the ServerHello is what shows the
// version was.
class OneShotTlsServer {
public:
    explicit OneShotTlsServer(int max_version) : ctx_(SSL_CTX_new(TLS_server_method())) {
        const net::TlsFiles& files = ulw::test::TestPki::shared().server();
        EXPECT_EQ(SSL_CTX_use_certificate_chain_file(ctx_.get(), files.certificate_chain.c_str()),
                  1);
        EXPECT_EQ(
            SSL_CTX_use_PrivateKey_file(ctx_.get(), files.private_key.c_str(), SSL_FILETYPE_PEM),
            1);
        EXPECT_EQ(SSL_CTX_set_min_proto_version(ctx_.get(), TLS1_2_VERSION), 1);
        EXPECT_EQ(SSL_CTX_set_max_proto_version(ctx_.get(), max_version), 1);
        SSL_CTX_set_msg_callback(ctx_.get(), &OneShotTlsServer::on_message);
        auto listener = net::listen_tcp({.port = 0, .loopback_only = true});
        EXPECT_TRUE(listener);
        listener_ = std::move(*listener);
        port_ = *net::local_port(listener_.get());
        thread_ = std::jthread([this] { serve(); });
    }

    [[nodiscard]] std::string url() const {
        return "https://127.0.0.1:" + std::to_string(port_) + "/";
    }
    // Once the client is done: the version of the ServerHello sent, 0 when none was.
    [[nodiscard]] int answered_with() {
        if (thread_.joinable()) {
            thread_.join();
        }
        return answered_.load();
    }
    // Once the client is done: whether its ClientHello arrived.
    [[nodiscard]] bool heard_hello() {
        if (thread_.joinable()) {
            thread_.join();
        }
        return hello_seen_.load();
    }

private:
    static void on_message(int write_p, int /*version*/, int content_type, const void* buf,
                           std::size_t len, SSL* ssl, void* /*arg*/) {
        if (content_type != SSL3_RT_HANDSHAKE || len == 0) {
            return;
        }
        const unsigned char type = *static_cast<const unsigned char*>(buf);
        if (write_p == 0 && type == SSL3_MT_CLIENT_HELLO) {
            hello_seen_.store(true);
        }
        if (write_p == 1 && type == SSL3_MT_SERVER_HELLO) {
            answered_.store(SSL_version(ssl));
        }
    }

    void serve() {
        pollfd p{.fd = listener_.get(), .events = POLLIN, .revents = 0};
        // The client connects at once; this bounds a broken test, it is not waited out.
        if (::poll(&p, 1, 10'000) != 1) {
            return;
        }
        const os::UniqueFd conn(::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!conn) {
            return;
        }
        const ulw::test::SslPtr ssl(SSL_new(ctx_.get()));
        SSL_set_fd(ssl.get(), conn.get());
        // Fails either way: on the version, or on the client's certificate alert.
        static_cast<void>(SSL_accept(ssl.get()));
    }

    // One server per test at a time, and the callback has no user pointer of its own.
    static inline std::atomic<int> answered_{0};
    static inline std::atomic<bool> hello_seen_{false};
    ulw::test::SslCtxPtr ctx_;
    os::UniqueFd listener_;
    std::uint16_t port_ = 0;
    std::jthread thread_;

public:
    static void reset() noexcept {
        answered_.store(0);
        hello_seen_.store(false);
    }
};

infra::curl::Result get(const std::string& url) {
    return infra::curl::perform({.method = infra::curl::Method::Get,
                                 .url = url,
                                 .headers = {},
                                 .max_body = 1024,
                                 .timeout = std::chrono::seconds(10)});
}

TEST(TlsFloor, AServerThatStopsAtTls12IsRefusedBeforeItAnswers) {
    OneShotTlsServer::reset();
    OneShotTlsServer server(TLS1_2_VERSION);
    const auto result = get(server.url());
    ASSERT_FALSE(result);
    // The client did reach the server and offer a handshake, which was then refused on its
    // version: not a connection that never happened.
    EXPECT_TRUE(server.heard_hello()) << result.error().detail;
    EXPECT_EQ(server.answered_with(), 0) << "the handshake went on at TLS 1.2";
    EXPECT_EQ(result.error().kind, FailureKind::Network) << result.error().detail;
}

TEST(TlsFloor, ATls13ServerIsAnsweredAtTls13) {
    OneShotTlsServer::reset();
    OneShotTlsServer server(TLS1_3_VERSION);
    const auto result = get(server.url());
    // Accepted as far as the certificate, which only the test CA vouches for.
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, FailureKind::Tls) << result.error().detail;
    EXPECT_EQ(server.answered_with(), TLS1_3_VERSION);
}

} // namespace
