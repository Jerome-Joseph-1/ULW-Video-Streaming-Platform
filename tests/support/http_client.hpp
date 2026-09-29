#pragma once

#include "os/unique_fd.hpp"

#include "tls_pki.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <memory>
#include <openssl/ssl.h>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>

namespace ulw::test {

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;

    [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
        const auto it = headers.find(std::string(name));
        return it == headers.end() ? std::nullopt : std::optional(it->second);
    }
    [[nodiscard]] std::optional<std::uint64_t> upload_offset() const {
        const auto h = header("upload-offset");
        if (!h) {
            return std::nullopt;
        }
        std::uint64_t v = 0;
        std::from_chars(std::to_address(h->begin()), std::to_address(h->end()), v);
        return v;
    }
};

// Where a test server listens, and the client context that reaches it through TLS; without
// one the client speaks plaintext.
struct Endpoint {
    std::uint16_t port = 0;
    SSL_CTX* tls = nullptr;
};

// A blocking HTTP/1.1 client for tests: one connection, requests in sequence.
class HttpClient {
public:
    explicit HttpClient(Endpoint endpoint) {
        const std::uint16_t port = endpoint.port;
        fd_ = os::UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        timeval tv{.tv_sec = 30, .tv_usec = 0};
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        const int one = 1;
        ::setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // connect() takes every address family through the generic sockaddr header.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
            fd_.reset();
        }
        if (fd_ && endpoint.tls != nullptr && !start_tls(endpoint.tls)) {
            ssl_.reset();
            fd_.reset();
        }
    }

    [[nodiscard]] bool connected() const { return static_cast<bool>(fd_); }
    [[nodiscard]] int fd() const { return fd_.get(); }
    [[nodiscard]] SSL* ssl() const { return ssl_.get(); }
    void close() {
        ssl_.reset();
        fd_.reset();
    }

    // One write, through TLS if the connection has it; 0 once the peer stops accepting within
    // the send timeout.
    std::size_t send_some(std::span<const std::byte> bytes) {
        if (ssl_) {
            std::size_t n = 0;
            return SSL_write_ex(ssl_.get(), bytes.data(), bytes.size(), &n) == 1 ? n : 0;
        }
        const ssize_t n = ::send(fd_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

    // Sends everything; false if the peer stopped accepting before the timeout.
    bool send_raw(std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const std::size_t n = send_some(bytes);
            if (n == 0) {
                return false;
            }
            bytes = bytes.subspan(n);
        }
        return true;
    }
    bool send_raw(std::string_view text) { return send_raw(std::as_bytes(std::span(text))); }

    bool send_request(std::string_view method, std::string_view path, std::string_view token,
                      std::span<const std::byte> body = {},
                      const std::map<std::string, std::string>& extra = {}) {
        std::string head =
            std::string(method) + " " + std::string(path) + " HTTP/1.1\r\nHost: test\r\n";
        if (!token.empty()) {
            head += "Authorization: Bearer " + std::string(token) + "\r\n";
        }
        for (const auto& [k, v] : extra) {
            head.append(k).append(": ").append(v).append("\r\n");
        }
        if (!body.empty() || method == "POST" || method == "PATCH") {
            head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        head += "\r\n";
        return send_raw(head) && send_raw(body);
    }

    // Reads one response; nullopt on timeout or close before a full response.
    std::optional<HttpResponse> read_response(bool head_request = false) {
        for (;;) {
            if (auto r = parse(head_request)) {
                return r;
            }
            std::array<char, 65536> buf{};
            const std::size_t n = receive(buf);
            if (n == 0) {
                return std::nullopt;
            }
            in_.append(buf.data(), n);
        }
    }

    std::optional<HttpResponse> request(std::string_view method, std::string_view path,
                                        std::string_view token,
                                        std::span<const std::byte> body = {},
                                        const std::map<std::string, std::string>& extra = {}) {
        if (!send_request(method, path, token, body, extra)) {
            return std::nullopt;
        }
        return read_response(method == "HEAD");
    }

    // True once the server has closed the connection (EOF, close_notify or reset) within
    // `limit`. A server that is still silent then, or that sends bytes instead, has not.
    bool closed_by_peer(std::chrono::milliseconds limit = std::chrono::seconds(10)) {
        if (!ssl_ || SSL_has_pending(ssl_.get()) == 0) {
            pollfd p{.fd = fd_.get(), .events = POLLIN, .revents = 0};
            if (::poll(&p, 1, static_cast<int>(limit.count())) != 1) {
                return false;
            }
        }
        std::array<char, 1> b{};
        errno = 0;
        if (ssl_) {
            std::size_t n = 0;
            if (SSL_read_ex(ssl_.get(), b.data(), b.size(), &n) == 1) {
                return false;
            }
            const int code = SSL_get_error(ssl_.get(), 0);
            // SO_RCVTIMEO expiring inside OpenSSL's read surfaces as a syscall error too.
            return code == SSL_ERROR_ZERO_RETURN || code == SSL_ERROR_SSL ||
                   (code == SSL_ERROR_SYSCALL && errno != EAGAIN && errno != EWOULDBLOCK);
        }
        const ssize_t n = ::recv(fd_.get(), b.data(), b.size(), 0);
        return n == 0 || (n < 0 && (errno == ECONNRESET || errno == EPIPE));
    }

private:
    // 0 on end of stream, error or timeout, whether the stream is TLS or not.
    std::size_t receive(std::span<char> into) {
        if (ssl_) {
            std::size_t n = 0;
            return SSL_read_ex(ssl_.get(), into.data(), into.size(), &n) == 1 ? n : 0;
        }
        const ssize_t n = ::recv(fd_.get(), into.data(), into.size(), 0);
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

    bool start_tls(SSL_CTX* ctx) {
        // OpenSSL writes to the socket with write(), which has no MSG_NOSIGNAL: a server that
        // closed first would kill the test process instead of failing the write.
        static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
        ssl_.reset(SSL_new(ctx));
        if (!ssl_ || SSL_set_fd(ssl_.get(), fd_.get()) != 1 || SSL_connect(ssl_.get()) != 1) {
            return false;
        }
        return SSL_version(ssl_.get()) != TLS1_3_VERSION || await_ticket();
    }

    // A TLS 1.3 client is done before the server has read its Finished. The server sends its
    // tickets only once it has, so a ticket means the server's side of the handshake is over
    // too, and a test that moves the clock next cannot catch it half way.
    bool await_ticket() {
        const int flags = ::fcntl(fd_.get(), F_GETFL);
        ::fcntl(fd_.get(), F_SETFL, flags | O_NONBLOCK);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool ticket = false;
        while (!ticket && std::chrono::steady_clock::now() < deadline) {
            pollfd p{.fd = fd_.get(), .events = POLLIN, .revents = 0};
            ::poll(&p, 1, 100);
            std::array<char, 1> b{};
            std::size_t n = 0;
            if (SSL_peek_ex(ssl_.get(), b.data(), b.size(), &n) != 1 &&
                SSL_get_error(ssl_.get(), 0) != SSL_ERROR_WANT_READ) {
                break;
            }
            ticket = SSL_SESSION_is_resumable(SSL_get0_session(ssl_.get())) == 1;
        }
        ::fcntl(fd_.get(), F_SETFL, flags);
        return ticket;
    }

    std::optional<HttpResponse> parse(bool head_request) {
        const std::size_t end = in_.find("\r\n\r\n");
        if (end == std::string::npos) {
            return std::nullopt;
        }
        HttpResponse r;
        const std::string_view head(in_.data(), end);
        const std::size_t line_end = head.find("\r\n");
        const std::string_view status_line = head.substr(0, line_end);
        // "HTTP/1.1 " is nine characters; the status code is the three after it.
        const std::string_view code =
            status_line.substr(std::min<std::size_t>(9, status_line.size()), 3);
        std::from_chars(std::to_address(code.begin()), std::to_address(code.end()), r.status);
        std::size_t pos = line_end == std::string_view::npos ? head.size() : line_end + 2;
        while (pos < head.size()) {
            std::size_t next = head.find("\r\n", pos);
            if (next == std::string_view::npos) {
                next = head.size();
            }
            const std::string_view line = head.substr(pos, next - pos);
            const std::size_t colon = line.find(':');
            std::string name(line.substr(0, colon));
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            r.headers[name] = std::string(value);
            pos = next + 2;
        }
        std::size_t length = 0;
        if (const auto cl = r.header("content-length"); cl && !head_request) {
            std::from_chars(std::to_address(cl->begin()), std::to_address(cl->end()), length);
        }
        if (in_.size() < end + 4 + length) {
            return std::nullopt;
        }
        r.body = in_.substr(end + 4, length);
        in_.erase(0, end + 4 + length);
        return r;
    }

    os::UniqueFd fd_;
    SslPtr ssl_;
    std::string in_;
};

} // namespace ulw::test
