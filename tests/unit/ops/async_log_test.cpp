#include "os/unique_fd.hpp"

#include "ops/async_log.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fcntl.h>
#include <gtest/gtest.h>
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

std::size_t count_lines(const std::string& text) {
    return static_cast<std::size_t>(std::ranges::count(text, '\n'));
}

TEST(AsyncLogSink, EveryLineQueuedBeforeDestructionIsWrittenInOrder) {
    Pipe p = make_pipe();
    {
        ops::AsyncLogSink sink(p.write.get(), 4096);
        for (int i = 0; i < 20; ++i) {
            sink.write("line " + std::to_string(i) + "\n");
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
        ops::AsyncLogSink sink(p.write.get(), 4096);
        // 100 KB against at most 4 KiB queued, 4 KiB being written and 4 KiB in the pipe:
        // getting through the loop at all shows the caller never waited for the reader.
        for (std::size_t i = 0; i < kLines; ++i) {
            sink.write(line);
        }
        dropped = sink.dropped();
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
        ops::AsyncLogSink sink(p.write.get(), 512);
        for (int i = 0; i < 5000; ++i) {
            sink.write("0123456789012345678901234567890123456789\n");
        }
        dropped = sink.dropped();
    }
    p.write.reset();
    reader.join();
    EXPECT_EQ(count_lines(got) + dropped, 5000U);
    EXPECT_EQ(got.size(), count_lines(got) * 41);
}

} // namespace
