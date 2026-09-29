#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdint>
#include <srt.h>
#include <string_view>

namespace ulw::test {

// A publisher, as LiveKit egress is to the packager: an SRT caller in live mode.
class SrtCaller {
public:
    SrtCaller() : socket_(::srt_create_socket()) {}
    ~SrtCaller() { ::srt_close(socket_); }
    SrtCaller(const SrtCaller&) = delete;
    SrtCaller& operator=(const SrtCaller&) = delete;
    SrtCaller(SrtCaller&&) = delete;
    SrtCaller& operator=(SrtCaller&&) = delete;

    // True when the handshake completed. An empty passphrase or stream id is not sent.
    [[nodiscard]] bool connect(std::uint16_t port, std::string_view passphrase,
                               std::string_view stream_id) const {
        const int live = SRTT_LIVE;
        // Refusals come back at once, but a listener that is gone never answers.
        const int patience_ms = 1000;
        ::srt_setsockflag(socket_, SRTO_TRANSTYPE, &live, sizeof live);
        ::srt_setsockflag(socket_, SRTO_CONNTIMEO, &patience_ms, sizeof patience_ms);
        if (!passphrase.empty()) {
            ::srt_setsockflag(socket_, SRTO_PASSPHRASE, passphrase.data(),
                              static_cast<int>(passphrase.size()));
        }
        if (!stream_id.empty()) {
            ::srt_setsockflag(socket_, SRTO_STREAMID, stream_id.data(),
                              static_cast<int>(stream_id.size()));
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API's own cast.
        return ::srt_connect(socket_, reinterpret_cast<const sockaddr*>(&address),
                             sizeof address) != SRT_ERROR;
    }

    [[nodiscard]] bool send(std::string_view payload) const {
        return ::srt_sendmsg(socket_, payload.data(), static_cast<int>(payload.size()), -1, 0) ==
               static_cast<int>(payload.size());
    }

private:
    SRTSOCKET socket_;
};

} // namespace ulw::test
