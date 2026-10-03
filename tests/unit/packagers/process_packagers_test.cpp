// ProcessPackagers with a stand-in for live_packager: a script that writes down what it was
// started with, then waits on a FIFO for the exit status the test hands it.
#include "infra/packagers/process_packagers.hpp"
#include "net/reactor_factory.hpp"
#include "os/system_clock.hpp"

#include "support/reactor_harness.hpp"
#include "support/temp_dir.hpp"

#include <sys/stat.h>

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

using core::ports::PackagerError;
using core::ports::PackagerState;
using infra::packagers::ProcessConfig;
using infra::packagers::ProcessPackagers;

constexpr std::string_view kStream = "0192f3a4-0000-7000-8000-0000000000bb";

class ProcessPackagersTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto r = net::make_reactor(net::ReactorKind::Epoll, clock, 64);
        ASSERT_TRUE(r);
        reactor = std::move(*r);
        script = dir.path() / "packager.sh";
        std::ofstream(script) << "#!/bin/sh\n"
                                 "env > \"$ULW_TEST_DIR/env.$ULW_STREAM_ID\"\n"
                                 "read code < \"$ULW_TEST_DIR/fifo\"\n"
                                 "exit \"$code\"\n";
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
        ASSERT_EQ(::mkfifo((dir.path() / "fifo").c_str(), 0600), 0);
        auto made = ProcessPackagers::create(
            *reactor, ProcessConfig{.binary = script.string(),
                                    .environment = {"ULW_TEST_DIR=" + dir.path().string(),
                                                    "ULW_LIVE_INGEST_PORT=9000"}});
        ASSERT_TRUE(made) << made.error();
        packagers = std::move(*made);
    }

    void TearDown() override {
        packagers.reset();
        reactor.reset();
    }

    std::expected<void, PackagerError> start(std::string_view stream = kStream) {
        std::optional<std::expected<void, PackagerError>> r;
        packagers->start({.stream = *core::LiveStreamId::parse(stream),
                          .owner = *core::UserId::parse("auth0|alice"),
                          .passphrase = "fake-passphrase-testtest123"},
                         [&](auto x) noexcept { r = x; });
        EXPECT_FALSE(r.has_value()) << "answered inside the call";
        EXPECT_TRUE(ulw::test::pump_until(*reactor, [&] { return r.has_value(); }));
        return r.value_or(std::unexpected(PackagerError::Unavailable));
    }

    std::expected<PackagerState, PackagerError> state(std::string_view stream = kStream) {
        std::optional<std::expected<PackagerState, PackagerError>> r;
        packagers->state(*core::LiveStreamId::parse(stream), [&](auto x) noexcept { r = x; });
        EXPECT_FALSE(r.has_value()) << "answered inside the call";
        EXPECT_TRUE(ulw::test::pump_until(*reactor, [&] { return r.has_value(); }));
        return r.value_or(std::unexpected(PackagerError::Unavailable));
    }

    // What the child wrote of its environment, once it has.
    std::string environment(std::string_view stream = kStream) {
        const auto file = dir.path() / ("env." + std::string(stream));
        EXPECT_TRUE(ulw::test::pump_until(*reactor, [&] {
            return std::filesystem::exists(file) && std::filesystem::file_size(file) > 0;
        }));
        const std::ifstream in(file);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // Hands the waiting child its exit status; blocks until it has opened the FIFO.
    void finish(int code) {
        const int fd = ::open((dir.path() / "fifo").c_str(), O_WRONLY | O_CLOEXEC);
        ASSERT_GE(fd, 0);
        const std::string line = std::to_string(code) + "\n";
        ASSERT_EQ(::write(fd, line.data(), line.size()), static_cast<ssize_t>(line.size()));
        ::close(fd);
    }

    ulw::test::TempDir dir{"ulw-packagers"};
    std::filesystem::path script;
    os::SystemClock clock;
    std::unique_ptr<net::IReactor> reactor;
    std::unique_ptr<ProcessPackagers> packagers;
};

TEST_F(ProcessPackagersTest, AChildGetsItsStreamAndNothingOfThisProcess) {
    EXPECT_EQ(state(), PackagerState::Absent);
    ASSERT_TRUE(start());
    const std::string env = environment();
    EXPECT_NE(env.find("ULW_STREAM_ID=" + std::string(kStream) + "\n"), std::string::npos);
    EXPECT_NE(env.find("ULW_STREAM_OWNER=auth0|alice\n"), std::string::npos);
    EXPECT_NE(env.find("ULW_LIVE_SRT_PASSPHRASE=fake-passphrase-testtest123\n"), std::string::npos);
    EXPECT_NE(env.find("ULW_LIVE_INGEST_PORT=9000\n"), std::string::npos);
    // The test runner's own environment stays here.
    EXPECT_EQ(env.find("GTEST"), std::string::npos);
    EXPECT_EQ(env.find("HOME="), std::string::npos);
    EXPECT_EQ(state(), PackagerState::Ready);
    // A second start for the stream starts nothing.
    ASSERT_TRUE(start());
    finish(0);
    ASSERT_TRUE(
        ulw::test::pump_until(*reactor, [&] { return state() == PackagerState::Finished; }));
}

TEST_F(ProcessPackagersTest, AChildThatFailsIsFailed) {
    ASSERT_TRUE(start());
    static_cast<void>(environment());
    finish(3);
    ASSERT_TRUE(ulw::test::pump_until(*reactor, [&] { return state() == PackagerState::Failed; }));
}

TEST_F(ProcessPackagersTest, ABinaryThatIsGoneIsRefused) {
    std::filesystem::remove(script);
    EXPECT_EQ(start(), std::unexpected(PackagerError::Refused));
    EXPECT_EQ(state(), PackagerState::Absent);
}

TEST(ProcessPackagersConfig, TheBinaryMustBeAnAbsoluteExecutablePath) {
    os::SystemClock clock;
    auto reactor = std::move(*net::make_reactor(net::ReactorKind::Epoll, clock, 64));
    EXPECT_FALSE(
        ProcessPackagers::create(*reactor, {.binary = "live_packager", .environment = {}}));
    EXPECT_FALSE(ProcessPackagers::create(
        *reactor, {.binary = "/nonexistent/live_packager", .environment = {}}));
    EXPECT_TRUE(ProcessPackagers::create(*reactor, {.binary = "/bin/sh", .environment = {}}));
}

} // namespace
