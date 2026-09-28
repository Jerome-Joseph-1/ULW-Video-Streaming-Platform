#include "os/unique_fd.hpp"

#include <array>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>
#include <utility>

namespace {

using os::UniqueFd;

bool is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

struct Pipe {
    int read_end = -1;
    int write_end = -1;
};

Pipe make_pipe() {
    std::array<int, 2> fds{-1, -1};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        ADD_FAILURE() << "pipe2 failed";
    }
    return {.read_end = fds[0], .write_end = fds[1]};
}

TEST(UniqueFd, DestructorClosesTheDescriptor) {
    const Pipe p = make_pipe();
    {
        const UniqueFd r(p.read_end);
        const UniqueFd w(p.write_end);
        EXPECT_TRUE(is_open(p.read_end));
    }
    EXPECT_FALSE(is_open(p.read_end));
    EXPECT_FALSE(is_open(p.write_end));
}

TEST(UniqueFd, MoveConstructionTransfersOwnership) {
    const Pipe p = make_pipe();
    const UniqueFd w(p.write_end);
    UniqueFd source(p.read_end);
    {
        const UniqueFd target(std::move(source));
        // The moved-from state is what is under test.
        // NOLINTNEXTLINE(bugprone-use-after-move)
        EXPECT_FALSE(source);
        EXPECT_EQ(target.get(), p.read_end);
    }
    EXPECT_FALSE(is_open(p.read_end));
}

TEST(UniqueFd, MoveAssignmentClosesTheDescriptorItReplaces) {
    const Pipe p = make_pipe();
    UniqueFd target(p.read_end);
    UniqueFd source(p.write_end);
    target = std::move(source);
    EXPECT_FALSE(is_open(p.read_end));
    EXPECT_TRUE(is_open(p.write_end));
    EXPECT_EQ(target.get(), p.write_end);
}

TEST(UniqueFd, SelfMoveAssignmentKeepsTheDescriptor) {
    const Pipe p = make_pipe();
    const UniqueFd w(p.write_end);
    UniqueFd fd(p.read_end);
    UniqueFd& alias = fd;
    fd = std::move(alias);
    EXPECT_TRUE(is_open(p.read_end));
    EXPECT_EQ(fd.get(), p.read_end);
}

TEST(UniqueFd, ReleaseHandsTheDescriptorBackOpen) {
    const Pipe p = make_pipe();
    const UniqueFd w(p.write_end);
    int released = -1;
    {
        UniqueFd fd(p.read_end);
        released = fd.release();
        EXPECT_FALSE(fd);
    }
    EXPECT_EQ(released, p.read_end);
    EXPECT_TRUE(is_open(p.read_end));
    ::close(p.read_end);
}

TEST(UniqueFd, ResetToTheOwnedDescriptorKeepsItOpen) {
    const Pipe p = make_pipe();
    const UniqueFd w(p.write_end);
    {
        UniqueFd fd(p.read_end);
        fd.reset(fd.get());
        EXPECT_TRUE(is_open(p.read_end));
        EXPECT_EQ(fd.get(), p.read_end);
    }
    EXPECT_FALSE(is_open(p.read_end));
}

TEST(UniqueFd, ResetToAnotherDescriptorClosesTheOldOne) {
    const Pipe p = make_pipe();
    {
        UniqueFd fd(p.read_end);
        fd.reset(p.write_end);
        EXPECT_FALSE(is_open(p.read_end));
        EXPECT_TRUE(is_open(p.write_end));
    }
    EXPECT_FALSE(is_open(p.write_end));
}

} // namespace
