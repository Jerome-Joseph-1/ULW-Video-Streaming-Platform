#include "net/signals.hpp"

#include <sys/signalfd.h>

#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace net {

namespace {

sigset_t shutdown_set() noexcept {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    return set;
}

} // namespace

std::expected<void, int> block_shutdown_signals() noexcept {
    const sigset_t set = shutdown_set();
    if (const int rc = ::pthread_sigmask(SIG_BLOCK, &set, nullptr); rc != 0) {
        return std::unexpected(rc);
    }
    // Writes to a vanished peer must fail with EPIPE, not kill the process. MSG_NOSIGNAL
    // covers our own sends; libraries doing their own writes need this.
    if (::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return std::unexpected(errno);
    }
    return {};
}

std::expected<std::unique_ptr<SignalWatcher>, int> SignalWatcher::create(IReactor& reactor,
                                                                         ISignalHandler& handler) {
    const sigset_t set = shutdown_set();
    os::UniqueFd fd{::signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC)};
    if (!fd) {
        return std::unexpected(errno);
    }
    const int raw = fd.get();
    auto watcher = std::make_unique<SignalWatcher>(reactor, handler, std::move(fd));
    if (auto r = reactor.watch(raw, Interest::Read, *watcher); !r) {
        return std::unexpected(r.error());
    }
    return watcher;
}

SignalWatcher::SignalWatcher(IReactor& reactor, ISignalHandler& handler, os::UniqueFd fd) noexcept
    : reactor_(reactor), handler_(handler), fd_(std::move(fd)) {}

SignalWatcher::~SignalWatcher() {
    reactor_.unwatch(fd_.get());
}

void SignalWatcher::on_ready(Interest /*ready*/) noexcept {
    signalfd_siginfo info{};
    while (::read(fd_.get(), &info, sizeof info) == static_cast<ssize_t>(sizeof info)) {
        switch (info.ssi_signo) {
        case SIGTERM:
        case SIGINT:
            handler_.on_signal(Signal::Terminate);
            break;
        case SIGHUP:
            handler_.on_signal(Signal::Reload);
            break;
        default:
            break;
        }
    }
}

} // namespace net
