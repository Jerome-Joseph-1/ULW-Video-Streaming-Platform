#include "heartbeat.hpp"
#include "support/temp_dir.hpp"

#include <sys/stat.h>

#include <array>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

// Back-dates `file` to the epoch, so any beat after it shows as a newer time.
void make_stale(const fs::path& file) {
    const std::array<timespec, 2> epoch{{{.tv_sec = 0, .tv_nsec = 0}, {.tv_sec = 0, .tv_nsec = 0}}};
    ASSERT_EQ(::utimensat(AT_FDCWD, file.c_str(), epoch.data(), AT_SYMLINK_NOFOLLOW), 0) << errno;
}

std::time_t mtime(const fs::path& file) {
    struct stat st {};
    return ::lstat(file.c_str(), &st) == 0 ? st.st_mtim.tv_sec : -1;
}

TEST(Heartbeat, TheFirstBeatCreatesAFileOnlyItsOwnerCanWrite) {
    const ulw::test::TempDir dir;
    const worker::Heartbeat heartbeat(dir.path() / "heartbeat");
    heartbeat.beat();
    ASSERT_TRUE(fs::is_regular_file(heartbeat.file()));
    EXPECT_EQ(fs::status(heartbeat.file()).permissions(),
              fs::perms::owner_read | fs::perms::owner_write);
}

TEST(Heartbeat, ABeatRefreshesAStaleFile) {
    const ulw::test::TempDir dir;
    const worker::Heartbeat heartbeat(dir.path() / "heartbeat");
    heartbeat.beat();
    make_stale(heartbeat.file());
    ASSERT_EQ(mtime(heartbeat.file()), 0);
    heartbeat.beat();
    EXPECT_GT(mtime(heartbeat.file()), 0);
}

TEST(Heartbeat, ALinkInItsPlaceIsNotFollowed) {
    const ulw::test::TempDir dir;
    const fs::path target = dir.path() / "elsewhere";
    std::ofstream(target) << "untouched";
    make_stale(target);
    fs::create_symlink(target, dir.path() / "heartbeat");
    const worker::Heartbeat heartbeat(dir.path() / "heartbeat");
    heartbeat.beat();
    EXPECT_EQ(mtime(target), 0);
    EXPECT_TRUE(fs::is_symlink(heartbeat.file()));
}

} // namespace
