#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "epoll_reactor.hpp"
#include "uring_reactor.hpp"

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {

using net::ReactorKind;

// Nothing is attached here; the descriptor table only has to exist.
constexpr std::size_t kSlots = 64;

TEST(ReactorFactory, KindNamesRoundTripAndUnknownNamesAreRefused) {
    for (const ReactorKind kind : {ReactorKind::IoUring, ReactorKind::Epoll}) {
        EXPECT_EQ(net::parse_reactor_kind(net::to_string(kind)), kind);
    }
    EXPECT_FALSE(net::parse_reactor_kind("kqueue"));
    EXPECT_FALSE(net::parse_reactor_kind(""));
}

TEST(ReactorFactory, EpollIsTakenAsRequestedWithoutFallback) {
    os::SystemClock clock;
    auto choice = net::make_reactor_with_fallback(ReactorKind::Epoll, clock, kSlots);
    ASSERT_TRUE(choice);
    EXPECT_EQ(choice->kind, ReactorKind::Epoll);
    EXPECT_FALSE(choice->fell_back_from_io_uring);
    EXPECT_NE(dynamic_cast<net::detail::EpollReactor*>(choice->reactor.get()), nullptr);
}

// The io_uring half of every parameterised suite needs the same kernel support.
TEST(ReactorFactory, IoUringIsTakenWhereTheKernelOffersIt) {
    os::SystemClock clock;
    auto choice = net::make_reactor_with_fallback(ReactorKind::IoUring, clock, kSlots);
    ASSERT_TRUE(choice);
    EXPECT_EQ(choice->kind, ReactorKind::IoUring);
    EXPECT_FALSE(choice->fell_back_from_io_uring);
    EXPECT_NE(dynamic_cast<net::detail::UringReactor*>(choice->reactor.get()), nullptr);
}

TEST(ReactorFactory, OnlyAnOutrightTwoInTheSysctlDisablesIoUring) {
    EXPECT_TRUE(net::detail::io_uring_disabled("2"));
    EXPECT_FALSE(net::detail::io_uring_disabled("0"));
    EXPECT_FALSE(net::detail::io_uring_disabled("1"));
    EXPECT_FALSE(net::detail::io_uring_disabled("12"));
    EXPECT_FALSE(net::detail::io_uring_disabled("2x"));
    EXPECT_FALSE(net::detail::io_uring_disabled(""));
}

// Answers io_uring_setup with EPERM, as Docker's default seccomp profile does, and lets every
// other call through.
bool refuse_io_uring_setup() {
    std::array<sock_filter, 4> filter{{
        {.code = BPF_LD | BPF_W | BPF_ABS, .jt = 0, .jf = 0, .k = offsetof(seccomp_data, nr)},
        {.code = BPF_JMP | BPF_JEQ | BPF_K, .jt = 0, .jf = 1, .k = __NR_io_uring_setup},
        {.code = BPF_RET | BPF_K, .jt = 0, .jf = 0, .k = SECCOMP_RET_ERRNO | EPERM},
        {.code = BPF_RET | BPF_K, .jt = 0, .jf = 0, .k = SECCOMP_RET_ALLOW},
    }};
    const sock_fprog program{.len = static_cast<unsigned short>(filter.size()),
                             .filter = filter.data()};
    return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
           ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

enum class FallbackOutcome : std::uint8_t {
    FellBackToEpoll,
    FilterRefused,
    NoReactor,
    WrongChoice
};

FallbackOutcome request_io_uring_under_seccomp() {
    if (!refuse_io_uring_setup()) {
        return FallbackOutcome::FilterRefused;
    }
    os::SystemClock clock;
    auto choice = net::make_reactor_with_fallback(ReactorKind::IoUring, clock, kSlots);
    if (!choice) {
        return FallbackOutcome::NoReactor;
    }
    const bool epoll = choice->kind == ReactorKind::Epoll &&
                       dynamic_cast<net::detail::EpollReactor*>(choice->reactor.get()) != nullptr;
    return epoll && choice->fell_back_from_io_uring == EPERM ? FallbackOutcome::FellBackToEpoll
                                                             : FallbackOutcome::WrongChoice;
}

// A seccomp filter cannot be lifted once installed, so it goes on in a child.
TEST(ReactorFactory, IoUringRefusedBySeccompFallsBackToEpoll) {
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::_exit(static_cast<int>(request_io_uring_under_seccomp()));
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << "child status " << status;
    EXPECT_EQ(static_cast<FallbackOutcome>(WEXITSTATUS(status)), FallbackOutcome::FellBackToEpoll);
}

} // namespace
