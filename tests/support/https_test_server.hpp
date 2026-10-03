#pragma once

#include "os/unique_fd.hpp"

#include "http_test_server.hpp"
#include "tls_pki.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ulw::test {

// HttpTestServer over TLS, with TestPki's identity for localhost and 127.0.0.1: one connection at
// a time, one request per connection, every reply closes. The handler runs on the server thread.
class HttpsTestServer {
public:
    using Handler = std::function<Reply(const ServedRequest&)>;

    explicit HttpsTestServer(Handler handler);
    ~HttpsTestServer();
    HttpsTestServer(const HttpsTestServer&) = delete;
    HttpsTestServer& operator=(const HttpsTestServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    // "https://localhost:<port>"
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] std::vector<ServedRequest> requests() const;
    [[nodiscard]] std::size_t request_count() const;
    // TCP connections accepted, whether or not a request followed.
    [[nodiscard]] std::size_t connections() const;

private:
    void serve(const std::stop_token& stop);
    void serve_connection(int fd);
    [[nodiscard]] bool wait_readable(int fd) const;

    Handler handler_;
    SslCtxPtr ctx_;
    os::UniqueFd listener_;
    os::UniqueFd stop_fd_;
    std::uint16_t port_ = 0;

    mutable std::mutex mutex_;
    std::vector<ServedRequest> requests_;
    std::size_t connections_ = 0;

    // Last: the thread must stop before anything it touches is destroyed.
    std::jthread thread_;
};

} // namespace ulw::test
