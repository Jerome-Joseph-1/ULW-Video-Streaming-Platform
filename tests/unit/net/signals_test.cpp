#include "net/reactor_factory.hpp"
#include "net/signals.hpp"
#include "os/system_clock.hpp"

#include "support/reactor_harness.hpp"

#include <csignal>
#include <cstring>
#include <ctime>
#include <gtest/gtest.h>
#include <memory>
#include <pthread.h>
#include <unistd.h>
#include <vector>

namespace {

using net::Signal;
using ulw::test::pump_until;

struct Recorder final : net::ISignalHandler {
    std::vector<Signal> received;
    void on_signal(Signal signal) noexcept override { received.push_back(signal); }
};

class SignalWatcherTest : public ::testing::TestWithParam<net::ReactorKind> {
protected:
    void SetUp() override {
        ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, nullptr, &saved_mask_), 0);
        ASSERT_EQ(::sigaction(SIGPIPE, nullptr, &saved_sigpipe_), 0);
        ASSERT_TRUE(net::block_shutdown_signals());
        auto r = net::make_reactor(GetParam(), clock_, 4096);
        ASSERT_TRUE(r) << "reactor setup failed: " << std::strerror(r.error());
        reactor_ = std::move(*r);
        auto w = net::SignalWatcher::create(*reactor_, recorder_);
        ASSERT_TRUE(w) << "signalfd setup failed: " << std::strerror(w.error());
        watcher_ = std::move(*w);
    }

    void TearDown() override {
        watcher_.reset();
        reactor_.reset();
        // One the watcher never read would take the process down as soon as it is unblocked.
        sigset_t shutdown{};
        sigemptyset(&shutdown);
        for (const int signo : {SIGTERM, SIGINT, SIGHUP}) {
            sigaddset(&shutdown, signo);
        }
        const timespec no_wait{};
        while (::sigtimedwait(&shutdown, nullptr, &no_wait) > 0) {
        }
        ::sigaction(SIGPIPE, &saved_sigpipe_, nullptr);
        ::pthread_sigmask(SIG_SETMASK, &saved_mask_, nullptr);
    }

    // Sent to the process, as a supervisor's signal is. The kernel would hand it to any thread
    // not blocking it; this binary runs on one thread, which block_shutdown_signals covers.
    bool deliver(int signo) {
        const std::size_t before = recorder_.received.size();
        return ::kill(::getpid(), signo) == 0 &&
               pump_until(*reactor_, [&] { return recorder_.received.size() > before; });
    }

    sigset_t saved_mask_{};
    struct sigaction saved_sigpipe_ {};
    os::SystemClock clock_;
    std::unique_ptr<net::IReactor> reactor_;
    Recorder recorder_;
    std::unique_ptr<net::SignalWatcher> watcher_;
};

TEST_P(SignalWatcherTest, TerminateAndInterruptBothAskForShutdown) {
    ASSERT_TRUE(deliver(SIGTERM));
    ASSERT_TRUE(deliver(SIGINT));
    EXPECT_EQ(recorder_.received, (std::vector{Signal::Terminate, Signal::Terminate}));
}

TEST_P(SignalWatcherTest, HangupAsksForReload) {
    ASSERT_TRUE(deliver(SIGHUP));
    EXPECT_EQ(recorder_.received, (std::vector{Signal::Reload}));
}

INSTANTIATE_TEST_SUITE_P(Reactors, SignalWatcherTest,
                         ::testing::Values(net::ReactorKind::IoUring, net::ReactorKind::Epoll),
                         ulw::test::reactor_name);

} // namespace
