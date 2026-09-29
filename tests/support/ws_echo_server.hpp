#pragma once

#include "codec/ws/decoder.hpp"
#include "core/util/time.hpp"
#include "net/reactor.hpp"
#include "net/signals.hpp"
#include "net/slab.hpp"

#include <cstddef>
#include <cstdint>

namespace ulw::test {

struct WsEchoOptions {
    core::Millis idle_timeout{10'000};
    std::uint64_t max_message_bytes = codec::ws::Decoder::kDefaultMaxMessageBytes;
    // As EchoOptions: stop reading from a peer whose own echo has piled up this far.
    std::size_t high_watermark = std::size_t{256} * 1024;
    std::size_t max_connections = 1'000;
    core::Millis drain_deadline{5'000};
};

// Accepts WebSocket upgrades and echoes every message: the vehicle for the Autobahn
// fuzzingclient. Text and Binary come back as they came, Ping is answered with Pong, a Close is
// returned with its status, and a protocol error is answered with a Close carrying the
// decoder's status. The server then sends FIN, and closes once the peer has too.
class WsEchoServer final : public net::IAcceptHandler,
                           public net::ISignalHandler,
                           public net::ITimerHandler {
public:
    WsEchoServer(net::IReactor& reactor, WsEchoOptions options);
    ~WsEchoServer() override;
    WsEchoServer(const WsEchoServer&) = delete;
    WsEchoServer& operator=(const WsEchoServer&) = delete;

    void on_accept(os::UniqueFd conn) noexcept override;
    void on_signal(net::Signal signal) noexcept override;
    void on_timeout() noexcept override;

    // Sends Close 1001 on every open connection and stops accepting.
    void begin_drain() noexcept;
    // Destroys sessions whose sockets the kernel has let go of. Call after every run_once.
    void reap() noexcept;
    [[nodiscard]] bool finished() const noexcept { return draining_ && sessions_.size() == 0; }
    [[nodiscard]] std::size_t connections() const noexcept { return sessions_.size(); }

private:
    class Session;

    net::IReactor& reactor_;
    WsEchoOptions options_;
    net::Slab<Session> sessions_;
    bool draining_ = false;
    net::TimerId drain_timer_;
};

} // namespace ulw::test
