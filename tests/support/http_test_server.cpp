#include "http_test_server.hpp"

#include "net/socket.hpp"

#include <sys/eventfd.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace ulw::test {

namespace {

// Request headers from libcurl are a few hundred bytes; anything past this is a bug.
constexpr std::size_t kMaxHead = std::size_t{64} << 10U;

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view trim(std::string_view s) noexcept {
    constexpr std::string_view kBlank = " \t";
    const std::size_t first = s.find_first_not_of(kBlank);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(kBlank) - first + 1);
}

std::string_view reason(int status) noexcept {
    switch (status) {
    case 200:
        return "OK";
    case 204:
        return "No Content";
    case 404:
        return "Not Found";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Status";
    }
}

bool send_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t n = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

} // namespace

std::optional<std::string_view> ServedRequest::header(std::string_view name) const {
    std::string lowered(name);
    std::ranges::transform(lowered, lowered.begin(), ascii_lower);
    for (const auto& [n, v] : headers) {
        if (n == lowered) {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

std::string_view ServedRequest::path() const {
    return std::string_view(target).substr(0, target.find('?'));
}

std::optional<std::string_view> ServedRequest::query(std::string_view name) const {
    const std::size_t q = target.find('?');
    if (q == std::string::npos) {
        return std::nullopt;
    }
    std::string_view rest = std::string_view(target).substr(q + 1);
    while (!rest.empty()) {
        const std::size_t amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
        const std::size_t eq = pair.find('=');
        if (pair.substr(0, eq) == name) {
            return eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
        }
    }
    return std::nullopt;
}

HttpTestServer::HttpTestServer(Handler handler) : handler_(std::move(handler)) {
    // Nonblocking, which the accept loop is fine with: it accepts only once poll says a
    // connection is waiting.
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

HttpTestServer::~HttpTestServer() {
    thread_.request_stop();
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(stop_fd_.get(), &one, sizeof one);
    thread_.join();
}

std::string HttpTestServer::base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

std::vector<ServedRequest> HttpTestServer::requests() const {
    const std::scoped_lock lock(mutex_);
    return requests_;
}

std::size_t HttpTestServer::request_count() const {
    const std::scoped_lock lock(mutex_);
    return requests_.size();
}

std::size_t HttpTestServer::body_bytes_in_progress() const {
    const std::scoped_lock lock(mutex_);
    return in_progress_;
}

bool HttpTestServer::wait_readable(int fd) const {
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

void HttpTestServer::record(ServedRequest request) {
    const std::scoped_lock lock(mutex_);
    requests_.push_back(std::move(request));
    in_progress_ = 0;
}

void HttpTestServer::serve(const std::stop_token& stop) {
    while (!stop.stop_requested() && wait_readable(listener_.get())) {
        const os::UniqueFd conn{::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
        if (conn) {
            serve_connection(conn.get(), stop);
        }
    }
}

void HttpTestServer::serve_connection(int fd, const std::stop_token& stop) {
    std::string buffer;
    std::array<char, 65536> chunk{};
    std::size_t head_end = std::string::npos;
    const auto read_more = [&]() -> bool {
        if (stop.stop_requested() || !wait_readable(fd)) {
            return false;
        }
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) {
            return false;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(n));
        return true;
    };
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

    request.body = buffer.substr(head_end + 4);
    request.complete = true;
    while (request.body.size() < content_length) {
        {
            const std::scoped_lock lock(mutex_);
            in_progress_ = request.body.size();
        }
        if (stop.stop_requested() || !wait_readable(fd)) {
            request.complete = false;
            break;
        }
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) {
            request.complete = false;
            break;
        }
        request.body.append(chunk.data(), static_cast<std::size_t>(n));
    }
    if (!request.complete) {
        record(std::move(request));
        return;
    }

    const Reply reply = handler_(request);
    record(std::move(request));
    std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " " +
                      std::string(reason(reply.status)) + "\r\nConnection: close\r\n";
    bool has_length = false;
    for (const auto& [name, value] : reply.headers) {
        std::string lowered(name);
        std::ranges::transform(lowered, lowered.begin(), ascii_lower);
        has_length = has_length || lowered == "content-length";
        out.append(name).append(": ").append(value).append("\r\n");
    }
    if (!has_length) {
        out.append("Content-Length: ").append(std::to_string(reply.body.size())).append("\r\n");
    }
    out.append("\r\n").append(reply.body);
    static_cast<void>(send_all(fd, out));
}

} // namespace ulw::test
