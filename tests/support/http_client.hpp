#pragma once

#include "os/unique_fd.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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

// A blocking HTTP/1.1 client for tests: one connection, requests in sequence.
class HttpClient {
public:
    explicit HttpClient(std::uint16_t port) {
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
    }

    [[nodiscard]] bool connected() const { return static_cast<bool>(fd_); }
    [[nodiscard]] int fd() const { return fd_.get(); }
    void close() { fd_.reset(); }

    // Sends everything; false if the peer stopped accepting before the timeout.
    bool send_raw(std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const ssize_t n = ::send(fd_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            bytes = bytes.subspan(static_cast<std::size_t>(n));
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
            const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
            if (n <= 0) {
                return std::nullopt;
            }
            in_.append(buf.data(), static_cast<std::size_t>(n));
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

    // True once the server has closed the connection (EOF or reset) within the timeout.
    bool closed_by_peer() {
        char b = 0;
        return ::recv(fd_.get(), &b, 1, 0) <= 0;
    }

private:
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
    std::string in_;
};

} // namespace ulw::test
