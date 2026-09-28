#pragma once

#include "net/reactor.hpp"

#include <expected>
#include <memory>

namespace net {

enum class Signal { Terminate, Reload };

class ISignalHandler {
public:
    virtual void on_signal(Signal signal) noexcept = 0;

protected:
    ~ISignalHandler() = default;
};

// Blocks SIGTERM, SIGINT and SIGHUP and ignores SIGPIPE. Must run before any thread is
// started: threads inherit the mask, so the signals queue for the signalfd instead of
// interrupting whichever thread the kernel happens to pick.
[[nodiscard]] std::expected<void, int> block_shutdown_signals() noexcept;

// SIGTERM and SIGINT arrive as Terminate, SIGHUP as Reload.
class SignalWatcher final : public IReadyHandler {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<SignalWatcher>, int>
    create(IReactor& reactor, ISignalHandler& handler);

    SignalWatcher(IReactor& reactor, ISignalHandler& handler, os::UniqueFd fd) noexcept;
    ~SignalWatcher();
    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

    void on_ready(Interest ready) noexcept override;

private:
    IReactor& reactor_;
    ISignalHandler& handler_;
    os::UniqueFd fd_;
};

} // namespace net
