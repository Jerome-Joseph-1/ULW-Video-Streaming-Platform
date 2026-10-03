#pragma once

#include "net/transport.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <openssl/ssl.h>
#include <string>
#include <string_view>

namespace ulw::test {

struct SslCtxFree {
    void operator()(SSL_CTX* ctx) const noexcept { SSL_CTX_free(ctx); }
};
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxFree>;

struct SslSessionFree {
    void operator()(SSL_SESSION* session) const noexcept { SSL_SESSION_free(session); }
};
// A session held by reference, from SSL_get1_session.
using SslSessionPtr = std::unique_ptr<SSL_SESSION, SslSessionFree>;

struct SslFree {
    void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
};
using SslPtr = std::unique_ptr<SSL, SslFree>;

// A certificate authority that exists only for this test process, and server identities it
// signs for localhost and 127.0.0.1, written as PEM files into a temporary directory. Made at
// run time so that no key, and nothing binary, is ever committed.
class TestPki {
public:
    // Built on first use and shared by every test in the process: key generation is the slow
    // part, and nothing a test does changes the CA.
    static TestPki& shared();

    TestPki();
    ~TestPki();
    TestPki(const TestPki&) = delete;
    TestPki& operator=(const TestPki&) = delete;

    // The CA's certificate, PEM: what a client that trusts this CA alone loads.
    [[nodiscard]] std::string ca_file() const { return (dir_ / "ca.pem").string(); }
    // The identity the gateway and transport tests serve.
    [[nodiscard]] const net::TlsFiles& server() const noexcept { return server_; }
    // A new key and a certificate for it naming `common_name`, under `stem` in the directory.
    // Writing over an existing stem replaces its files, as a certificate renewal would.
    [[nodiscard]] net::TlsFiles issue(std::string_view stem, std::string_view common_name);

    // Trusts this CA alone and checks that the server is localhost. `max_version` 0 leaves the
    // newest protocol on.
    [[nodiscard]] SslCtxPtr client_context(int max_version = 0) const;

private:
    struct Keys;

    std::mutex mutex_;
    std::filesystem::path dir_;
    std::unique_ptr<Keys> keys_;
    net::TlsFiles server_;
    long serial_ = 1;
};

// The subject common name of the certificate the peer presented, or "" when there is none.
[[nodiscard]] std::string peer_common_name(const SSL* ssl);

} // namespace ulw::test
