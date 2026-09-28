#pragma once

#include "net/reactor.hpp"
#include "net/signals.hpp"
#include "net/slab.hpp"

#include <cstddef>

namespace ulw::test {

struct EchoOptions {
    core::Millis idle_timeout{10'000};
    // Stop reading from a peer once this much of its own echo is still unsent; its bytes
    // then wait in the kernel instead of in our queue.
    std::size_t high_watermark = std::size_t{256} * 1024;
    std::size_t max_connections = 10'000;
    core::Millis drain_deadline{30'000};
};

// The M2 acceptance vehicle: echoes every byte, closes idle peers, drains on SIGTERM.
class EchoServer final : public net::IAcceptHandler,
                         public net::ISignalHandler,
                         public net::ITimerHandler {
public:
    EchoServer(net::IReactor& reactor, EchoOptions options);
    ~EchoServer();
    EchoServer(const EchoServer&) = delete;
    EchoServer& operator=(const EchoServer&) = delete;

    void on_accept(os::UniqueFd conn) noexcept override;
    void on_signal(net::Signal signal) noexcept override;
    void on_timeout() noexcept override;

    void begin_drain() noexcept;
    // Destroys sessions whose sockets the kernel has let go of. Call after every run_once.
    void reap() noexcept;
    [[nodiscard]] bool finished() const noexcept { return draining_ && sessions_.size() == 0; }
    [[nodiscard]] std::size_t connections() const noexcept { return sessions_.size(); }
    [[nodiscard]] std::size_t rejected() const noexcept { return rejected_; }

private:
    class Session;

    void retire(net::Slab<Session>::Handle handle) noexcept;

    net::IReactor& reactor_;
    EchoOptions options_;
    net::Slab<Session> sessions_;
    bool draining_ = false;
    std::size_t rejected_ = 0;
    net::TimerId drain_timer_;
};

} // namespace ulw::test
