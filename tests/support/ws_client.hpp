#pragma once

#include "codec/ws/encoder.hpp"
#include "codec/ws/frame.hpp"
#include "core/util/parse.hpp"
#include "os/unique_fd.hpp"

#include "fake_random.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace ulw::test {

// A blocking WebSocket client for tests against a server on loopback: the opening handshake,
// masked text frames out, and the server's unmasked frames in. Pings are answered; anything
// the client does not expect ends the connection, which the test then sees as a missing
// message.
class WsClient {
public:
    // Upgrades on `path` with the given extra header lines ("Name: value\r\n" each). nullopt
    // with the response status line in `refusal` when the server says anything but 101.
    // `pipelined` goes out in the same write, right behind the request, as if the client sent
    // its first frames without waiting for the 101. A `receive_buffer` fixes the socket's (Linux
    // doubles it), for a reader whose kernel should hold a known amount of what it has not read.
    static std::optional<WsClient> connect(std::uint16_t port, std::string_view path,
                                           std::string_view headers, std::string* refusal,
                                           int receive_buffer) {
        return connect(port, path, headers, refusal, {}, receive_buffer);
    }

    static std::optional<WsClient> connect(std::uint16_t port, std::string_view path,
                                           std::string_view headers, std::string* refusal,
                                           std::span<const std::byte> pipelined = {},
                                           int receive_buffer = 0) {
        WsClient c(port, receive_buffer);
        if (!c.fd_) {
            return std::nullopt;
        }
        const std::string request =
            "GET " + std::string(path) +
            " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n" +
            std::string(headers) + "\r\n";
        const auto head_out = as_bytes(request);
        std::vector<std::byte> out(head_out.begin(), head_out.end());
        out.insert(out.end(), pipelined.begin(), pipelined.end());
        if (!c.write_all(out)) {
            return std::nullopt;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::size_t end = std::string::npos;
        while ((end = c.text_in().find("\r\n\r\n")) == std::string::npos) {
            if (!c.read_more(deadline)) {
                if (refusal != nullptr) {
                    *refusal = c.text_in();
                }
                return std::nullopt;
            }
        }
        const std::string head = c.text_in().substr(0, end);
        c.in_.erase(c.in_.begin(), c.in_.begin() + static_cast<std::ptrdiff_t>(end + 4));
        if (!head.starts_with("HTTP/1.1 101")) {
            if (refusal != nullptr) {
                *refusal = head.substr(0, head.find("\r\n"));
            }
            return std::nullopt;
        }
        return c;
    }

    bool send_text(std::string_view text) {
        std::vector<std::byte> out;
        return append(out, codec::ws::Opcode::Text, text) && write_all(out);
    }

    // Appends one masked frame, for a test that wants several in a single write.
    bool append(std::vector<std::byte>& out, codec::ws::Opcode opcode, std::string_view payload) {
        const auto bytes = std::as_bytes(std::span{payload});
        codec::ws::ClientEncoder encoder(random_);
        return encoder
            .encode({.opcode = opcode,
                     .fin = true,
                     .payload = {bytes.begin(), bytes.end()},
                     .close_code = codec::ws::CloseCode::NoStatus},
                    out)
            .has_value();
    }

    bool send_raw(std::span<const std::byte> bytes) { return write_all(bytes); }

    // The status of the Close the server sends, skipping whatever comes before it.
    std::optional<std::uint16_t> close_status(std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (true) {
            if (auto frame = take_frame()) {
                if (frame->first != codec::ws::Opcode::Close) {
                    continue;
                }
                if (frame->second.size() < 2) {
                    return std::nullopt;
                }
                return static_cast<std::uint16_t>(
                    (static_cast<unsigned char>(frame->second[0]) << 8U) |
                    static_cast<unsigned char>(frame->second[1]));
            }
            if (!read_more(deadline)) {
                return std::nullopt;
            }
        }
    }

    // The next frame of any kind, as it came: nothing is answered.
    std::optional<std::pair<codec::ws::Opcode, std::string>>
    next_frame(std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (true) {
            if (auto frame = take_frame()) {
                return frame;
            }
            if (!read_more(deadline)) {
                return std::nullopt;
            }
        }
    }

    // The next text message, or nullopt once `limit` passes or the connection ends.
    std::optional<std::string> next_text(std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (true) {
            if (auto frame = take_frame()) {
                switch (frame->first) {
                case codec::ws::Opcode::Text:
                    return std::move(frame->second);
                case codec::ws::Opcode::Ping:
                    pong(frame->second);
                    continue;
                case codec::ws::Opcode::Pong:
                    continue;
                case codec::ws::Opcode::Binary:
                case codec::ws::Opcode::Close:
                case codec::ws::Opcode::Continuation:
                    fd_.reset();
                    return std::nullopt;
                }
            }
            if (!read_more(deadline)) {
                return std::nullopt;
            }
        }
    }

    // Reads at most `most` bytes of whatever has arrived, without waiting, and keeps them for
    // the calls above: a reader far slower than what it is sent. Returns how many it read.
    std::size_t read_at_most(std::size_t most) {
        std::vector<std::byte> buf(most);
        const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), MSG_DONTWAIT);
        if (n <= 0) {
            return 0;
        }
        in_.insert(in_.end(), buf.begin(), buf.begin() + n);
        return static_cast<std::size_t>(n);
    }

    [[nodiscard]] bool connected() const noexcept { return static_cast<bool>(fd_); }
    // How the connection ended, once it has: 0 for the server's FIN, otherwise the errno of the
    // read that found it gone (ECONNRESET for a reset).
    [[nodiscard]] int error() const noexcept { return error_; }

private:
    WsClient(std::uint16_t port, int receive_buffer) {
        fd_ = os::UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        if (!fd_) {
            return;
        }
        const int one = 1;
        ::setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        // Before connect(): the window scale is agreed in the handshake.
        // A buffer that could not be fixed leaves no socket: the caller's connect fails, rather
        // than a test running on a buffer it did not ask for.
        if (receive_buffer > 0 && ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                                               sizeof receive_buffer) != 0) {
            fd_.reset();
            return;
        }
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

    static std::span<const std::byte> as_bytes(std::string_view text) {
        return std::as_bytes(std::span{text});
    }

    [[nodiscard]] std::string text_in() const {
        std::string out;
        out.reserve(in_.size());
        for (const std::byte b : in_) {
            out += static_cast<char>(b);
        }
        return out;
    }

    bool write_all(std::span<const std::byte> bytes) {
        while (!bytes.empty() && fd_) {
            const ssize_t n = ::send(fd_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            bytes = bytes.subspan(static_cast<std::size_t>(n));
        }
        return bytes.empty();
    }

    bool read_more(std::chrono::steady_clock::time_point deadline) {
        if (!fd_) {
            return false;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return false;
        }
        pollfd pfd{.fd = fd_.get(), .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0) {
            return false;
        }
        std::array<std::byte, 65536> buf{};
        const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
        if (n <= 0) {
            error_ = n < 0 ? errno : 0;
            fd_.reset();
            return false;
        }
        in_.insert(in_.end(), buf.begin(), buf.begin() + n);
        return true;
    }

    // A whole server frame, unmasked and unfragmented, as chat_server sends them.
    std::optional<std::pair<codec::ws::Opcode, std::string>> take_frame() {
        if (in_.size() < 2) {
            return std::nullopt;
        }
        const auto b0 = std::to_integer<std::uint8_t>(in_[0]);
        const auto b1 = std::to_integer<std::uint8_t>(in_[1]);
        std::size_t header = 2;
        std::uint64_t length = b1 & 0x7FU;
        // RFC 6455 section 5.2: 126 means a 16-bit length follows, 127 a 64-bit one.
        std::size_t extended = 0;
        if (length == 126) {
            extended = 2;
        } else if (length == 127) {
            extended = 8;
        }
        if (in_.size() < header + extended) {
            return std::nullopt;
        }
        if (extended != 0) {
            length = 0;
            for (std::size_t i = 0; i < extended; ++i) {
                length = (length << 8U) | std::to_integer<std::uint8_t>(in_[header + i]);
            }
            header += extended;
        }
        if (in_.size() < header + length) {
            return std::nullopt;
        }
        std::string payload;
        for (std::size_t i = 0; i < length; ++i) {
            payload += static_cast<char>(in_[header + i]);
        }
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(header + length));
        return std::pair{static_cast<codec::ws::Opcode>(b0 & 0x0FU), std::move(payload)};
    }

    void pong(const std::string& payload) {
        std::vector<std::byte> out;
        const auto bytes = as_bytes(payload);
        codec::ws::ClientEncoder encoder(random_);
        if (encoder.encode({.opcode = codec::ws::Opcode::Pong,
                            .fin = true,
                            .payload = {bytes.begin(), bytes.end()},
                            .close_code = codec::ws::CloseCode::NoStatus},
                           out)) {
            write_all(out);
        }
    }

    os::UniqueFd fd_;
    std::vector<std::byte> in_;
    int error_ = 0;
    FakeRandom random_;
};

struct PlainResponse {
    // 0 when there was no answer.
    int status = 0;
    std::string body;
};

// A plain GET, read until the server closes, as chat_server does after every probe.
inline PlainResponse http_get(std::uint16_t port, std::string_view path) {
    const os::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return {};
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // connect() takes every address family through the generic sockaddr header.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        return {};
    }
    const std::string request = "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    if (::send(fd.get(), request.data(), request.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(request.size())) {
        return {};
    }
    std::string response;
    std::array<char, 4096> buf{};
    pollfd pfd{.fd = fd.get(), .events = POLLIN, .revents = 0};
    while (::poll(&pfd, 1, 5'000) > 0) {
        const ssize_t n = ::recv(fd.get(), buf.data(), buf.size(), 0);
        if (n <= 0) {
            break;
        }
        response.append(buf.data(), static_cast<std::size_t>(n));
    }
    // "HTTP/1.1 200 ..."
    constexpr std::size_t kCodeAt = 9;
    const std::size_t end = response.find("\r\n\r\n");
    if (response.size() < kCodeAt + 3 || end == std::string::npos) {
        return {};
    }
    return {.status = core::parse_integer<int>(response.substr(kCodeAt, 3)).value_or(0),
            .body = response.substr(end + 4)};
}

} // namespace ulw::test
