#include "scratch.hpp"
#include "support/temp_dir.hpp"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using ulw::test::TempDir;

constexpr std::string_view kMissingRoot =
    " is not a directory: make it, owned by the user the packager runs as with mode 0700, or "
    "name another";

void touch(const fs::path& path) {
    std::ofstream(path) << "left over";
}

::mode_t mode_of(const fs::path& path) {
    struct stat st {};
    EXPECT_EQ(::lstat(path.c_str(), &st), 0) << path;
    return st.st_mode & 07777;
}

// A directory of the host's, owned by a user who is neither this process's nor root, that this
// process may open: a service's own under /var. Empty when the host has none.
fs::path someone_elses_directory() {
    for (const char* const top : {"/var/cache", "/var/lib", "/var/spool", "/var/log", "/home"}) {
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(top, ec)) {
            struct stat st {};
            if (::lstat(entry.path().c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid != 0 &&
                st.st_uid != ::geteuid() && ::access(entry.path().c_str(), R_OK | X_OK) == 0) {
                return entry.path();
            }
        }
    }
    return {};
}

TEST(ClearScratch, MakesTheScratchAndMediaDirectoriesUnderAnExistingRoot) {
    const TempDir root("ulw-scratch");
    const fs::path scratch = root.path() / "show";
    ASSERT_TRUE(live::clear_scratch(scratch));
    EXPECT_TRUE(fs::is_directory(scratch / "media"));
    EXPECT_TRUE(fs::is_empty(scratch / "media"));
}

TEST(ClearScratch, EmptiesWhatAnEarlierRunLeft) {
    const TempDir root("ulw-scratch");
    const fs::path scratch = root.path() / "show";
    fs::create_directories(scratch / "media" / "old");
    touch(scratch / "media" / "old" / "seg-1.ts");
    touch(scratch / "outbox");
    ASSERT_TRUE(live::clear_scratch(scratch));
    EXPECT_TRUE(fs::is_empty(scratch / "media"));
    EXPECT_EQ(std::distance(fs::directory_iterator(scratch), fs::directory_iterator()), 1);
}

// A scratch directory that is a symbolic link is replaced by a directory; what it pointed at is
// left as it was.
TEST(ClearScratch, ReplacesALinkWithoutTouchingWhatItPointsAt) {
    const TempDir root("ulw-scratch");
    const TempDir elsewhere("ulw-scratch-elsewhere");
    touch(elsewhere.path() / "keep");
    const fs::path scratch = root.path() / "show";
    fs::create_directory_symlink(elsewhere.path(), scratch);
    ASSERT_TRUE(live::clear_scratch(scratch));
    EXPECT_FALSE(fs::is_symlink(scratch));
    EXPECT_TRUE(fs::is_directory(scratch / "media"));
    EXPECT_TRUE(fs::exists(elsewhere.path() / "keep"));
}

TEST(ClearScratch, AFileInTheScratchPlaceIsReplacedByADirectory) {
    const TempDir root("ulw-scratch");
    const fs::path scratch = root.path() / "show";
    touch(scratch);
    ASSERT_TRUE(live::clear_scratch(scratch));
    EXPECT_TRUE(fs::is_directory(scratch / "media"));
}

TEST(ClearScratch, TheScratchDirectoryIsOwnerOnlyWhateverTheUmask) {
    const TempDir root("ulw-scratch");
    const fs::path scratch = root.path() / "show";
    const ::mode_t saved = ::umask(0);
    const auto cleared = live::clear_scratch(scratch);
    ::umask(saved);
    ASSERT_TRUE(cleared) << cleared.error();
    EXPECT_EQ(mode_of(scratch), 0700U);
}

TEST(ClearScratch, AScratchDirectoryOthersCouldReachIsMadeOwnerOnly) {
    const TempDir root("ulw-scratch");
    const fs::path scratch = root.path() / "show";
    fs::create_directory(scratch);
    ASSERT_EQ(::chmod(scratch.c_str(), 0777), 0);
    const auto cleared = live::clear_scratch(scratch);
    ASSERT_TRUE(cleared) << cleared.error();
    EXPECT_EQ(mode_of(scratch), 0700U);
}

// A root that is a symbolic link is refused before anything is removed or made where it points.
TEST(ClearScratch, ARootThatIsASymbolicLinkIsRefusedAndWhereItPointsIsLeft) {
    const TempDir parent("ulw-scratch");
    const TempDir elsewhere("ulw-scratch-elsewhere");
    fs::create_directory(elsewhere.path() / "show");
    touch(elsewhere.path() / "show" / "keep");
    const fs::path root = parent.path() / "link";
    fs::create_directory_symlink(elsewhere.path(), root);
    const auto cleared = live::clear_scratch(root / "show");
    ASSERT_FALSE(cleared);
    EXPECT_EQ(cleared.error(),
              root.string() + ": not a directory (a symbolic link or a file is in its place)");
    EXPECT_TRUE(fs::exists(elsewhere.path() / "show" / "keep"));
    EXPECT_FALSE(fs::exists(elsewhere.path() / "show" / "media"));
}

// A root neither ours nor root's is refused before anything in it is touched. Root makes one;
// anyone else looks for one of the host's.
TEST(ClearScratch, ARootAnotherUserOwnsIsRefused) {
    const TempDir parent("ulw-scratch");
    fs::path root;
    if (::geteuid() == 0) {
        root = parent.path() / "theirs";
        fs::create_directory(root);
        fs::create_directory(root / "show");
        touch(root / "show" / "keep");
        ASSERT_EQ(::chmod(root.c_str(), 0777), 0);
        ASSERT_EQ(::chown(root.c_str(), 65534, 65534), 0);
    } else {
        root = someone_elses_directory();
        if (root.empty()) {
            GTEST_SKIP() << "not root, and no directory of another user's to try instead";
        }
    }
    struct stat st {};
    ASSERT_EQ(::lstat(root.c_str(), &st), 0) << root;
    const bool had_show = fs::exists(root / "show");
    const auto cleared = live::clear_scratch(root / "show");
    ASSERT_FALSE(cleared);
    EXPECT_EQ(cleared.error(), root.string() + ": owned by uid " + std::to_string(st.st_uid) +
                                   ", neither this process's user nor root, who alone may hold "
                                   "the directory it keeps its own in");
    EXPECT_EQ(fs::exists(root / "show"), had_show);
    if (::geteuid() == 0) {
        EXPECT_TRUE(fs::exists(root / "show" / "keep"));
    }
}

// A stream directory another user owns, in a root of ours, is refused and left as it was. Only
// root can hand one to another user.
TEST(ClearScratch, AScratchDirectoryAnotherUserOwnsIsRefusedAndLeft) {
    if (::geteuid() != 0) {
        GTEST_SKIP() << "needs root to give a directory to another user";
    }
    const TempDir root("ulw-scratch");
    const fs::path scratch = root.path() / "show";
    fs::create_directory(scratch);
    touch(scratch / "keep");
    ASSERT_EQ(::chmod(scratch.c_str(), 0755), 0);
    ASSERT_EQ(::chown(scratch.c_str(), 65534, 65534), 0);
    const auto cleared = live::clear_scratch(scratch);
    ASSERT_FALSE(cleared);
    EXPECT_EQ(cleared.error(),
              scratch.string() + ": owned by uid 65534, not by this process's user (uid 0)");
    EXPECT_TRUE(fs::exists(scratch / "keep"));
    EXPECT_EQ(mode_of(scratch), 0755U);
}

TEST(ClearScratch, AMissingRootIsNamedAndNotMade) {
    const TempDir parent("ulw-scratch");
    const fs::path root = parent.path() / "absent";
    const auto cleared = live::clear_scratch(root / "show");
    ASSERT_FALSE(cleared);
    EXPECT_EQ(cleared.error(), root.string() + std::string(kMissingRoot));
    EXPECT_FALSE(fs::exists(root));
}

TEST(ClearScratch, ARootThatIsAFileIsNotADirectory) {
    const TempDir parent("ulw-scratch");
    const fs::path root = parent.path() / "file";
    touch(root);
    const auto cleared = live::clear_scratch(root / "show");
    ASSERT_FALSE(cleared);
    EXPECT_EQ(cleared.error(), root.string() + std::string(kMissingRoot));
    EXPECT_TRUE(fs::is_regular_file(root));
}

// Anything but a missing root is reported with the system's reason: here a link to itself,
// which no lookup can resolve (ELOOP), whoever runs the test.
TEST(ClearScratch, ARootThatCannotBeLookedUpIsReportedWithTheReason) {
    const TempDir parent("ulw-scratch");
    const fs::path root = parent.path() / "loop";
    fs::create_symlink(root, root);
    const auto cleared = live::clear_scratch(root / "show");
    ASSERT_FALSE(cleared);
    EXPECT_EQ(cleared.error(),
              root.string() + ": " +
                  std::make_error_code(std::errc::too_many_symbolic_link_levels).message());
}

// A scratch name no file system accepts (longer than NAME_MAX) fails the clearing, and the
// error names the scratch directory.
TEST(ClearScratch, AScratchThatCannotBeClearedIsReportedWithTheReason) {
    const TempDir root("ulw-scratch");
    const fs::path scratch = root.path() / std::string(512, 's');
    const auto cleared = live::clear_scratch(scratch);
    ASSERT_FALSE(cleared);
    EXPECT_EQ(cleared.error(), scratch.string() + ": " +
                                   std::make_error_code(std::errc::filename_too_long).message());
}

// A root that is a directory but where nothing can be made: /proc refuses a new entry even to
// root, so the failure to make the scratch directory is seen whoever runs the test.
TEST(ClearScratch, AScratchThatCannotBeMadeIsReportedWithTheReason) {
    const fs::path scratch = "/proc/ulw-scratch-test";
    ASSERT_TRUE(fs::is_directory(scratch.parent_path()));
    const auto cleared = live::clear_scratch(scratch);
    ASSERT_FALSE(cleared);
    EXPECT_TRUE(cleared.error().starts_with(scratch.string() + ": ")) << cleared.error();
    EXPECT_FALSE(fs::exists(scratch));
}

} // namespace
