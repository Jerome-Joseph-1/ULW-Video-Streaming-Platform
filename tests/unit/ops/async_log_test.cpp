#include "os/unique_fd.hpp"

#include "ops/async_log.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

struct Pipe {
    os::UniqueFd read;
    os::UniqueFd write;
};

Pipe make_pipe() {
    std::array<int, 2> fds{};
    EXPECT_EQ(::pipe2(fds.data(), O_CLOEXEC), 0);
    return {.read = os::UniqueFd{fds[0]}, .write = os::UniqueFd{fds[1]}};
}

std::string read_all(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    while (true) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n <= 0) {
            return out;
        }
        out.append(buf.data(), static_cast<std::size_t>(n));
    }
}

// Long enough never to cut a flush short where a reader is draining the pipe.
constexpr core::Millis kFlushLimit{10'000};

std::unique_ptr<ops::AsyncLogSink> make_sink(int fd, std::size_t capacity, core::Millis limit) {
    auto sink = ops::AsyncLogSink::create(fd, capacity, limit);
    EXPECT_TRUE(sink.has_value());
    return sink ? std::move(*sink) : nullptr;
}

std::size_t count_lines(const std::string& text) {
    return static_cast<std::size_t>(std::ranges::count(text, '\n'));
}

TEST(AsyncLogSink, EveryLineQueuedBeforeDestructionIsWrittenInOrder) {
    Pipe p = make_pipe();
    {
        auto sink = make_sink(p.write.get(), 4096, kFlushLimit);
        for (int i = 0; i < 20; ++i) {
            sink->write("line " + std::to_string(i) + "\n");
        }
    }
    p.write.reset();
    std::string expected;
    for (int i = 0; i < 20; ++i) {
        expected += "line " + std::to_string(i) + "\n";
    }
    EXPECT_EQ(read_all(p.read.get()), expected);
}

TEST(AsyncLogSink, AStalledReaderCostsLinesNotTheWriter) {
    Pipe p = make_pipe();
    // One page of pipe, so the writing thread blocks almost at once and stays blocked.
    ASSERT_GE(::fcntl(p.write.get(), F_SETPIPE_SZ, 4096), 4096);
    const std::string line = std::string(99, 'x') + "\n";
    constexpr std::size_t kLines = 1000;
    std::uint64_t dropped = 0;
    std::string drained;
    std::optional<std::jthread> reader;
    {
        auto sink = make_sink(p.write.get(), 4096, kFlushLimit);
        // 100 KB against at most 4 KiB queued, 4 KiB being written and 4 KiB in the pipe:
        // getting through the loop at all shows the caller never waited for the reader.
        for (std::size_t i = 0; i < kLines; ++i) {
            sink->write(line);
        }
        dropped = sink->dropped();
        EXPECT_GE(dropped, kLines - (std::size_t{3} * 4096 / line.size()));
        reader.emplace([&] { drained = read_all(p.read.get()); });
    }
    p.write.reset();
    reader.reset();
    EXPECT_EQ(count_lines(drained) + dropped, kLines);
}

TEST(AsyncLogSink, NoLineIsSplitOrLost) {
    Pipe p = make_pipe();
    std::string got;
    std::jthread reader([&] { got = read_all(p.read.get()); });
    std::uint64_t dropped = 0;
    {
        auto sink = make_sink(p.write.get(), 512, kFlushLimit);
        for (int i = 0; i < 5000; ++i) {
            sink->write("0123456789012345678901234567890123456789\n");
        }
        dropped = sink->dropped();
    }
    p.write.reset();
    reader.join();
    EXPECT_EQ(count_lines(got) + dropped, 5000U);
    EXPECT_EQ(got.size(), count_lines(got) * 41);
}

TEST(AsyncLogSink, DestructionGivesUpOnAReaderThatNeverComes) {
    const Pipe p = make_pipe();
    ASSERT_GE(::fcntl(p.write.get(), F_SETPIPE_SZ, 4096), 4096);
    // A full pipe that nobody reads.
    const std::string filler(4096, 'f');
    ASSERT_EQ(::write(p.write.get(), filler.data(), filler.size()), 4096);
    const auto started = std::chrono::steady_clock::now();
    {
        auto sink = make_sink(p.write.get(), 4096, core::Millis{50});
        sink->write("one line\n");
        sink->write("another\n");
        // Both lines were queued, and close() is the only place left to say they were lost.
        EXPECT_EQ(sink->close(), 2U);
        sink->write("after close\n");
        EXPECT_EQ(sink->dropped(), 3U);
    }
    // Without the limit the destructor would wait for a reader forever.
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_GE(took, std::chrono::milliseconds(40));
    EXPECT_LT(took, std::chrono::seconds(5));
}

TEST(AsyncLogSink, NoEventfdNoSink) {
    // Without the eventfd, close() could not wake a thread waiting on a stalled reader, and
    // the exit would wait for that reader forever; the sink is refused instead. A limit of no
    // descriptors makes the eventfd fail; the child keeps it from the rest of the tests.
    EXPECT_EXIT(
        {
            rlimit none{};
            static_cast<void>(::getrlimit(RLIMIT_NOFILE, &none));
            none.rlim_cur = 0;
            static_cast<void>(::setrlimit(RLIMIT_NOFILE, &none));
            const auto sink = ops::AsyncLogSink::create(STDERR_FILENO, 4096, kFlushLimit);
            std::_Exit(!sink && sink.error() == EMFILE ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

} // namespace
