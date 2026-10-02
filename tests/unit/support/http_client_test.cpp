// The blocking test client itself: what the gateway and chat tests take a failed read by it to
// mean depends on it failing only when the server does.
#include "os/unique_fd.hpp"

#include "support/eventually.hpp"
#include "support/http_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

std::atomic<int> interruptions = 0;

void count_interruption(int /*signal*/) noexcept {
    interruptions.fetch_add(1);
}

// A loopback listener on a port of the kernel's choosing.
struct Listener {
    os::UniqueFd fd;
    std::uint16_t port = 0;
};

std::optional<Listener> listen_on_loopback() {
    os::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return std::nullopt;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof addr;
    // bind() and getsockname() take every address family through the generic header.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(fd.get(), 1) != 0 ||
        ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return std::nullopt;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return Listener{.fd = std::move(fd), .port = ntohs(addr.sin_port)};
}

// Whether thread `tid` of this process is blocked in system call `number`: the first field of
// its /proc syscall file, which reads "running" while it is not in one.
// nullopt when the file cannot be read, as where /proc is restricted or the kernel lacks it.
std::optional<long> current_syscall(pid_t tid) {
    std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/syscall");
    long current = -1;
    if (!(in >> current)) {
        return std::nullopt;
    }
    return current;
}

bool blocked_in(pid_t tid, long number) {
    return current_syscall(tid) == number;
}

// Installs a handler for SIGUSR1 while alive, and puts the one before it back.
class InterruptionHandler {
public:
    InterruptionHandler() {
        struct sigaction action {};
        action.sa_handler = count_interruption;
        sigemptyset(&action.sa_mask);
        installed_ = ::sigaction(SIGUSR1, &action, &before_) == 0;
    }
    ~InterruptionHandler() { ::sigaction(SIGUSR1, &before_, nullptr); }
    InterruptionHandler(const InterruptionHandler&) = delete;
    InterruptionHandler& operator=(const InterruptionHandler&) = delete;
    InterruptionHandler(InterruptionHandler&&) = delete;
    InterruptionHandler& operator=(InterruptionHandler&&) = delete;
    [[nodiscard]] bool installed() const noexcept { return installed_; }

private:
    struct sigaction before_ {};
    bool installed_ = false;
};

// A socket wait with SO_RCVTIMEO is never restarted after a signal (signal(7)), whatever the
// handler's flags: it fails with EINTR. So does one when the process is stopped and continued,
// by a debugger, a tracer or a frozen cgroup, with no handler at all. The client makes the call
// again; before it did, it took the interruption for the server closing the connection.
TEST(HttpClient, AReadInterruptedByASignalIsMadeAgain) {
    // The wait for the reader to block is read from /proc; without it there is no telling
    // when to interrupt it.
    if (!current_syscall(static_cast<pid_t>(::syscall(SYS_gettid)))) {
        GTEST_SKIP() << "cannot read /proc/self/task/<tid>/syscall";
    }
    const InterruptionHandler handler;
    ASSERT_TRUE(handler.installed());
    auto listener = listen_on_loopback();
    ASSERT_TRUE(listener);
    ulw::test::HttpClient client({.port = listener->port});
    const os::UniqueFd server{::accept4(listener->fd.get(), nullptr, nullptr, SOCK_CLOEXEC)};
    ASSERT_TRUE(server);

    std::atomic<pid_t> reader_tid = 0;
    auto response = std::async(std::launch::async, [&] {
        reader_tid = static_cast<pid_t>(::syscall(SYS_gettid));
        return client.read_response();
    });
    ASSERT_TRUE(ulw::test::eventually(
        [&] { return reader_tid != 0 && blocked_in(reader_tid, SYS_recvfrom); }));
    const int before = interruptions.load();
    ASSERT_EQ(::syscall(SYS_tgkill, ::getpid(), reader_tid.load(), SIGUSR1), 0);
    ASSERT_TRUE(ulw::test::eventually([&] { return interruptions.load() > before; }));

    constexpr std::string_view kAnswer = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    ASSERT_EQ(::send(server.get(), kAnswer.data(), kAnswer.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(kAnswer.size()));
    const auto answered = response.get();
    ASSERT_TRUE(answered) << "the interrupted read was taken for a closed connection";
    EXPECT_EQ(answered->status, 200);
    EXPECT_EQ(answered->body, "ok");
}

} // namespace
