#pragma once

#include "os/unique_fd.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ulw::test {

struct ServedRequest {
    std::string method;
    // Path and query exactly as sent.
    std::string target;
    // Names lowercased, in arrival order.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // False when the client went away before its Content-Length was all there.
    bool complete = false;

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
    [[nodiscard]] std::string_view path() const;
    [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const;
};

struct Reply {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// A deliberately small HTTP/1.1 peer on its own blocking thread: one connection at a time,
// one request per connection, and every reply closes. Enough to script the other end of a
// client under test; the handler runs on the server thread.
class HttpTestServer {
public:
    using Handler = std::function<Reply(const ServedRequest&)>;

    explicit HttpTestServer(Handler handler);
    ~HttpTestServer();
    HttpTestServer(const HttpTestServer&) = delete;
    HttpTestServer& operator=(const HttpTestServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    // "http://127.0.0.1:<port>"
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] std::vector<ServedRequest> requests() const;
    [[nodiscard]] std::size_t request_count() const;
    // Body bytes read so far from the connection being served, 0 between connections.
    [[nodiscard]] std::size_t body_bytes_in_progress() const;

private:
    void serve(const std::stop_token& stop);
    void serve_connection(int fd, const std::stop_token& stop);
    // Waits for `fd` or the stop signal; false once stopping.
    [[nodiscard]] bool wait_readable(int fd) const;
    void record(ServedRequest request);

    Handler handler_;
    os::UniqueFd listener_;
    os::UniqueFd stop_fd_;
    std::uint16_t port_ = 0;

    mutable std::mutex mutex_;
    std::vector<ServedRequest> requests_;
    std::size_t in_progress_ = 0;

    // Last member: the thread must stop before anything it touches is destroyed.
    std::jthread thread_;
};

} // namespace ulw::test
