#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "epoll_reactor.hpp"
#include "uring_reactor.hpp"

#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <liburing.h>
#include <memory>
#include <poll.h>
#include <unistd.h>
#include <vector>

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

// CAP_IPC_LOCK exempts a process from the locked-memory charge, so root drops it to be charged
// as CI's runner is.
bool drop_ipc_lock() {
    __user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> caps{};
    if (::syscall(SYS_capget, &header, caps.data()) != 0) {
        return false;
    }
    caps[0].effective &= ~(1U << static_cast<unsigned>(CAP_IPC_LOCK));
    return ::syscall(SYS_capset, &header, caps.data()) == 0;
}

// The kernel's and systemd's default, and CI's runners'.
constexpr rlim_t kLockedLimit = rlim_t{8} << 20U;
// The observer's own limit: room for the reactors' rings (about 420 KiB each) and a dozen more
// that the user's other processes may hold (other tests under ctest -j), and short of
// kLockedLimit.
constexpr rlim_t kObserverLimit = kLockedLimit / 4 * 3;
constexpr std::size_t kReactorsStarted = 2;
// Registering a buffer pins its pages and charges them to the user's locked memory;
// unregistering gives them back at once.
constexpr std::size_t kObserverBytes = std::size_t{16} * 1024;

enum class LockedMemoryOutcome : std::uint8_t {
    LeftRoom,
    TookTheLimit,
    ReactorRefused,
    CapabilityKept,
    LimitUnavailable,
    ObserverUnavailable
};

const char* describe(LockedMemoryOutcome outcome) {
    switch (outcome) {
    case LockedMemoryOutcome::LeftRoom:
        return "left room";
    case LockedMemoryOutcome::TookTheLimit:
        return "charged the user's locked memory far past its rings while starting";
    case LockedMemoryOutcome::ReactorRefused:
        return "the reactor itself was refused";
    case LockedMemoryOutcome::CapabilityKept:
        return "CAP_IPC_LOCK could not be dropped";
    case LockedMemoryOutcome::LimitUnavailable:
        return "RLIMIT_MEMLOCK could not be set";
    case LockedMemoryOutcome::ObserverUnavailable:
        return "the observer failed";
    }
    return "unknown";
}

bool set_locked_limit(rlim_t soft) {
    rlimit limit{};
    if (::getrlimit(RLIMIT_MEMLOCK, &limit) != 0) {
        return false;
    }
    limit.rlim_cur = soft;
    limit.rlim_max = std::max(limit.rlim_max, soft);
    return ::setrlimit(RLIMIT_MEMLOCK, &limit) == 0;
}

// Another process of the same user, under kObserverLimit: the counter it is checked against is
// the user's, the limit its own. Registers and unregisters a buffer until `stop` reaches end of
// file, and exits with whether a registration was refused: whether, meanwhile, the user's
// charge went past kObserverLimit. Signals `ready` once it has tried.
LockedMemoryOutcome observe(int stop, int ready) {
    if (!set_locked_limit(kObserverLimit)) {
        return LockedMemoryOutcome::LimitUnavailable;
    }
    io_uring ring{};
    if (io_uring_queue_init(1, &ring, 0) != 0) {
        return LockedMemoryOutcome::ObserverUnavailable;
    }
    std::vector<std::byte> buffer(kObserverBytes);
    const iovec iov{.iov_base = buffer.data(), .iov_len = buffer.size()};
    bool refused = false;
    bool working = true;
    for (bool first = true; working; first = false) {
        const int rc = io_uring_register_buffers(&ring, &iov, 1);
        if (rc == 0) {
            working = io_uring_unregister_buffers(&ring) == 0;
        } else {
            refused = refused || rc == -ENOMEM;
            working = rc == -ENOMEM;
        }
        if (first) {
            const char byte = 0;
            working = working && ::write(ready, &byte, 1) == 1;
        }
        pollfd p{.fd = stop, .events = POLLIN, .revents = 0};
        if (::poll(&p, 1, 0) != 0) {
            break;
        }
    }
    io_uring_queue_exit(&ring);
    if (!working) {
        return LockedMemoryOutcome::ObserverUnavailable;
    }
    return refused ? LockedMemoryOutcome::TookTheLimit : LockedMemoryOutcome::LeftRoom;
}

LockedMemoryOutcome start_reactors_while_observed() {
    if (!drop_ipc_lock()) {
        return LockedMemoryOutcome::CapabilityKept;
    }
    if (!set_locked_limit(kLockedLimit)) {
        return LockedMemoryOutcome::LimitUnavailable;
    }
    std::array<int, 2> stop{};
    std::array<int, 2> ready{};
    if (::pipe2(stop.data(), O_CLOEXEC) != 0 || ::pipe2(ready.data(), O_CLOEXEC) != 0) {
        return LockedMemoryOutcome::ObserverUnavailable;
    }
    const pid_t observer = ::fork();
    if (observer < 0) {
        return LockedMemoryOutcome::ObserverUnavailable;
    }
    if (observer == 0) {
        ::close(stop[1]);
        ::_exit(static_cast<int>(observe(stop[0], ready[1])));
    }
    ::close(stop[0]);
    ::close(ready[1]);
    char byte = 0;
    const bool watching = ::read(ready[0], &byte, 1) == 1;
    os::SystemClock clock;
    std::vector<std::unique_ptr<net::detail::UringReactor>> reactors;
    bool created = true;
    for (std::size_t i = 0; watching && created && i < kReactorsStarted; ++i) {
        auto reactor = net::detail::UringReactor::create(clock, kSlots);
        created = reactor.has_value();
        if (created) {
            reactors.push_back(std::move(*reactor));
        }
    }
    ::close(stop[1]);
    int status = 0;
    if (::waitpid(observer, &status, 0) != observer || !WIFEXITED(status) || !watching) {
        return LockedMemoryOutcome::ObserverUnavailable;
    }
    if (!created) {
        return LockedMemoryOutcome::ReactorRefused;
    }
    return static_cast<LockedMemoryOutcome>(WEXITSTATUS(status));
}

// Since 6.14 every ring's pages are charged to a locked-memory counter that all of a user's
// processes share, as is every zero-copy send in flight. A reactor that takes that counter to
// the limit, even for a moment, refuses every other reactor of its user its ring with ENOMEM:
// another test's under ctest -j, or another shard's. Starting one charges its rings and no
// more. The limit and the capability change for good, so they change in a child.
TEST(ReactorFactory, StartingAnIoUringReactorChargesTheUsersLockedMemoryOnlyItsRings) {
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::_exit(static_cast<int>(start_reactors_while_observed()));
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << "child status " << status;
    const auto outcome = static_cast<LockedMemoryOutcome>(WEXITSTATUS(status));
    if (outcome == LockedMemoryOutcome::LimitUnavailable) {
        GTEST_SKIP() << "the hard RLIMIT_MEMLOCK is below 8 MiB and cannot be raised";
    }
    EXPECT_EQ(outcome, LockedMemoryOutcome::LeftRoom) << describe(outcome);
}

} // namespace
