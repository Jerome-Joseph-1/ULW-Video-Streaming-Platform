
#include "ops/notify.hpp"
#include "support/fake_notify.hpp"
#include "support/temp_dir.hpp"

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <unistd.h>

namespace {

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
    const ulw::test::FakeNotifySocket manager(path);
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
    const ulw::test::FakeNotifySocket manager(std::string(1, '\0') + name);
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
