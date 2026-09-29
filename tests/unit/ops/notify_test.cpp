#include "os/unique_fd.hpp"

#include "ops/notify.hpp"
#include "support/temp_dir.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <map>
#include <string>

namespace {

// Stands in for the service manager: a datagram socket at the path NOTIFY_SOCKET names.
class FakeManager {
public:
    explicit FakeManager(const std::string& address) {
        fd_.reset(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(static_cast<void*>(addr.sun_path), address.data(), address.size());
        const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + address.size());
        // bind() takes every address family through the generic header.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        bound_ = ::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), len) == 0;
    }

    [[nodiscard]] bool bound() const noexcept { return bound_; }

    // The next datagram, or empty when none is waiting.
    [[nodiscard]] std::string receive() const {
        std::array<char, 512> buf{};
        const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
        return n <= 0 ? std::string{} : std::string(buf.data(), static_cast<std::size_t>(n));
    }

private:
    os::UniqueFd fd_;
    bool bound_ = false;
};

ops::Lookup env_of(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string(name));
        return it == vars.end() ? std::nullopt : std::optional(it->second);
    };
}

TEST(Notifier, NothingToDoOutsideANotifyUnit) {
    const auto n = ops::Notifier::from_env(env_of({}), 42);
    ASSERT_TRUE(n);
    EXPECT_FALSE(n->has_value());
}

TEST(Notifier, ReadyStoppingAndWatchdogReachAPathSocket) {
    const ulw::test::TempDir dir("ulw-notify");
    const std::string path = (dir.path() / "notify").string();
    const FakeManager manager(path);
    ASSERT_TRUE(manager.bound());
    auto n = ops::Notifier::from_env(env_of({{"NOTIFY_SOCKET", path}}), 42);
    ASSERT_TRUE(n && n->has_value());
    const ops::Notifier& notifier = **n;
    notifier.ready();
    notifier.watchdog();
    notifier.stopping();
    EXPECT_EQ(manager.receive(), "READY=1");
    EXPECT_EQ(manager.receive(), "WATCHDOG=1");
    EXPECT_EQ(manager.receive(), "STOPPING=1");
    EXPECT_EQ(manager.receive(), "");
    EXPECT_FALSE(notifier.watchdog_interval());
}

TEST(Notifier, AnAbstractSocketIsNamedWithALeadingAt) {
    const std::string name = "ulw-notify-test-" + std::to_string(::getpid());
    const FakeManager manager(std::string(1, '\0') + name);
    ASSERT_TRUE(manager.bound());
    auto n = ops::Notifier::from_env(env_of({{"NOTIFY_SOCKET", "@" + name}}), 42);
    ASSERT_TRUE(n && n->has_value());
    (*n)->ready();
    EXPECT_EQ(manager.receive(), "READY=1");
}

TEST(Notifier, TheWatchdogIsPingedAtHalfItsTimeoutAndOnlyForItsPid) {
    const std::string path = "/run/unused";
    auto mine = ops::Notifier::from_env(
        env_of({{"NOTIFY_SOCKET", path}, {"WATCHDOG_USEC", "30000000"}, {"WATCHDOG_PID", "42"}}),
        42);
    ASSERT_TRUE(mine && mine->has_value());
    EXPECT_EQ((*mine)->watchdog_interval(), core::Millis{15'000});
    auto theirs = ops::Notifier::from_env(
        env_of({{"NOTIFY_SOCKET", path}, {"WATCHDOG_USEC", "30000000"}, {"WATCHDOG_PID", "7"}}),
        42);
    ASSERT_TRUE(theirs && theirs->has_value());
    EXPECT_FALSE((*theirs)->watchdog_interval());
}

TEST(Notifier, AMalformedEnvironmentIsAnError) {
    EXPECT_FALSE(ops::Notifier::from_env(env_of({{"NOTIFY_SOCKET", "relative"}}), 1));
    EXPECT_FALSE(ops::Notifier::from_env(
        env_of({{"NOTIFY_SOCKET", "/run/x"}, {"WATCHDOG_USEC", "soon"}}), 1));
    EXPECT_FALSE(
        ops::Notifier::from_env(env_of({{"NOTIFY_SOCKET", "/" + std::string(200, 'a')}}), 1));
}

TEST(Notifier, AManagerThatIsGoneIsAnErrorNotACrash) {
    auto n = ops::Notifier::from_env(env_of({{"NOTIFY_SOCKET", "/nonexistent/notify"}}), 1);
    ASSERT_TRUE(n && n->has_value());
    EXPECT_FALSE((*n)->send("READY=1"));
}

} // namespace
