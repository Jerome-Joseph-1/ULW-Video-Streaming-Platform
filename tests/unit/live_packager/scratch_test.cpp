#include "scratch.hpp"
#include "support/temp_dir.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using ulw::test::TempDir;

constexpr std::string_view kMissingRoot =
    " is not a directory: make it, owned by the user the packager runs as with mode 0700, or "
    "name another";

void touch(const fs::path& path) {
    std::ofstream(path) << "left over";
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
