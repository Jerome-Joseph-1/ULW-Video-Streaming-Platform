#include "https_test_server.hpp"

#include "net/socket.hpp"

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

namespace ulw::test {

namespace {

constexpr std::size_t kMaxHead = std::size_t{64} << 10U;

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view trim(std::string_view s) noexcept {
    const std::size_t first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

} // namespace

HttpsTestServer::HttpsTestServer(Handler handler)
    : handler_(std::move(handler)), ctx_(SSL_CTX_new(TLS_server_method())) {
    const net::TlsFiles& files = TestPki::shared().server();
    if (!ctx_ ||
        SSL_CTX_use_certificate_chain_file(ctx_.get(), files.certificate_chain.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx_.get(), files.private_key.c_str(), SSL_FILETYPE_PEM) != 1) {
        throw std::runtime_error("test server: tls identity");
    }
    auto listener = net::listen_tcp({.port = 0, .loopback_only = true, .reuse_port = false});
    stop_fd_ = os::UniqueFd{::eventfd(0, EFD_CLOEXEC)};
    if (!listener || !stop_fd_) {
        throw std::runtime_error("test server: socket");
    }
    listener_ = std::move(*listener);
    const auto port = net::local_port(listener_.get());
    if (!port) {
        throw std::runtime_error("test server: bind");
    }
    port_ = *port;
    thread_ = std::jthread([this](const std::stop_token& stop) { serve(stop); });
}

HttpsTestServer::~HttpsTestServer() {
    thread_.request_stop();
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(stop_fd_.get(), &one, sizeof one);
    thread_.join();
}

std::string HttpsTestServer::base_url() const {
    return "https://localhost:" + std::to_string(port_);
}

std::vector<ServedRequest> HttpsTestServer::requests() const {
    const std::scoped_lock lock(mutex_);
    return requests_;
}

std::size_t HttpsTestServer::request_count() const {
    const std::scoped_lock lock(mutex_);
    return requests_.size();
}

std::size_t HttpsTestServer::connections() const {
    const std::scoped_lock lock(mutex_);
    return connections_;
}

bool HttpsTestServer::wait_readable(int fd) const {
    std::array<pollfd, 2> fds{pollfd{.fd = fd, .events = POLLIN, .revents = 0},
                              pollfd{.fd = stop_fd_.get(), .events = POLLIN, .revents = 0}};
    while (true) {
        const int n = ::poll(fds.data(), fds.size(), -1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n > 0 && fds[1].revents == 0;
    }
}

void HttpsTestServer::serve(const std::stop_token& stop) {
    while (!stop.stop_requested() && wait_readable(listener_.get())) {
        const os::UniqueFd conn{::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
        if (!conn) {
            continue;
        }
        {
            const std::scoped_lock lock(mutex_);
            ++connections_;
        }
        // A client that stops mid-handshake cannot hold the thread past this.
        const timeval limit{.tv_sec = 5, .tv_usec = 0};
        ::setsockopt(conn.get(), SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
        serve_connection(conn.get());
    }
}

void HttpsTestServer::serve_connection(int fd) {
    const SslPtr ssl(SSL_new(ctx_.get()));
    SSL_set_fd(ssl.get(), fd);
    if (SSL_accept(ssl.get()) != 1) {
        return;
    }
    std::string buffer;
    std::array<char, 16384> chunk{};
    const auto read_more = [&]() -> bool {
        if (SSL_pending(ssl.get()) == 0 && !wait_readable(fd)) {
            return false;
        }
        const int n = SSL_read(ssl.get(), chunk.data(), static_cast<int>(chunk.size()));
        if (n <= 0) {
            return false;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(n));
        return true;
    };
    std::size_t head_end = std::string::npos;
    while ((head_end = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (buffer.size() > kMaxHead || !read_more()) {
            return;
        }
    }
    ServedRequest request;
    std::string_view head = std::string_view(buffer).substr(0, head_end);
    const std::size_t line_end = head.find("\r\n");
    const std::string_view request_line = head.substr(0, line_end);
    const std::size_t sp1 = request_line.find(' ');
    const std::size_t sp2 = request_line.find(' ', sp1 + 1);
    request.method = std::string(request_line.substr(0, sp1));
    request.target = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
    head = line_end == std::string_view::npos ? std::string_view{} : head.substr(line_end + 2);
    std::uint64_t content_length = 0;
    while (!head.empty()) {
        const std::size_t eol = head.find("\r\n");
        const std::string_view line = head.substr(0, eol);
        head = eol == std::string_view::npos ? std::string_view{} : head.substr(eol + 2);
        const std::size_t colon = line.find(':');
        std::string name(line.substr(0, colon));
        std::ranges::transform(name, name.begin(), ascii_lower);
        const std::string_view value = trim(line.substr(colon + 1));
        if (name == "content-length") {
            std::from_chars(value.data(), value.data() + value.size(), content_length);
        }
        request.headers.emplace_back(std::move(name), std::string(value));
    }
    const std::size_t body_start = head_end + 4;
    while (buffer.size() - body_start < content_length) {
        if (!read_more()) {
            return;
        }
    }
    request.body = buffer.substr(body_start, content_length);
    request.complete = true;
    const Reply reply = handler_(request);
    {
        const std::scoped_lock lock(mutex_);
        requests_.push_back(std::move(request));
    }
    std::string out =
        "HTTP/1.1 " + std::to_string(reply.status) + " Status\r\nConnection: close\r\n";
    for (const auto& [name, value] : reply.headers) {
        out.append(name).append(": ").append(value).append("\r\n");
    }
    out.append("Content-Length: ").append(std::to_string(reply.body.size())).append("\r\n\r\n");
    out.append(reply.body);
    static_cast<void>(SSL_write(ssl.get(), out.data(), static_cast<int>(out.size())));
    static_cast<void>(SSL_shutdown(ssl.get()));
}

} // namespace ulw::test
