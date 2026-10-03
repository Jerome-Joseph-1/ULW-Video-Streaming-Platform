// The helper's check of the program it is asked to run, called here directly; the refusals as
// the helper reports them, by exit code and message, are in sandbox_test.cpp.
#include "program_check.hpp"
#include "support/temp_dir.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using infra::ffmpeg::sandbox::check_program;

class ProgramCheck : public ::testing::Test {
protected:
    // Canonical, so that every path built from it is absolute and in normal form.
    [[nodiscard]] fs::path dir() const { return fs::canonical(dir_.path()); }

    [[nodiscard]] fs::path file(const std::string& name, fs::perms perms) const {
        const fs::path path = dir() / name;
        std::ofstream(path) << "#!/bin/sh\nexit 0\n";
        fs::permissions(path, perms);
        return path;
    }

private:
    ulw::test::TempDir dir_{"ulw-program-check"};
};

TEST_F(ProgramCheck, AnExecutableFileByItsAbsolutePathMayRun) {
    EXPECT_TRUE(check_program(file("tool", fs::perms::owner_all).string()));
}

TEST_F(ProgramCheck, ASymbolicLinkToAnExecutableFileIsFollowed) {
    const fs::path link = dir() / "link";
    fs::create_symlink(file("tool", fs::perms::owner_all), link);
    EXPECT_TRUE(check_program(link.string()));
}

TEST_F(ProgramCheck, ABareNameIsNotFoundAndNoPathIsSearched) {
    const auto checked = check_program("sh");
    ASSERT_FALSE(checked);
    EXPECT_TRUE(checked.error().not_found);
    EXPECT_EQ(checked.error().reason, "not a path, and no PATH is searched");
}

TEST_F(ProgramCheck, ARelativePathIsRefused) {
    const auto checked = check_program("bin/sh");
    ASSERT_FALSE(checked);
    EXPECT_FALSE(checked.error().not_found);
    EXPECT_EQ(checked.error().reason, "not an absolute path");
}

TEST_F(ProgramCheck, APathThroughDotDotIsRefusedEvenWhereItLeadsToAProgram) {
    const fs::path tool = file("tool", fs::perms::owner_all);
    const std::string through = (dir() / ".." / dir().filename() / "tool").string();
    ASSERT_TRUE(fs::equivalent(through, tool));
    const auto checked = check_program(through);
    ASSERT_FALSE(checked);
    EXPECT_FALSE(checked.error().not_found);
    EXPECT_EQ(checked.error().reason, "has a .. component");
}

TEST_F(ProgramCheck, APathNotInNormalFormIsRefused) {
    const fs::path tool = file("tool", fs::perms::owner_all);
    for (const std::string& path : {(dir() / "." / "tool").string(), dir().string() + "//tool"}) {
        ASSERT_TRUE(fs::equivalent(path, tool)) << path;
        const auto checked = check_program(path);
        ASSERT_FALSE(checked) << path;
        EXPECT_FALSE(checked.error().not_found) << path;
        EXPECT_EQ(checked.error().reason, "not in normal form") << path;
    }
}

TEST_F(ProgramCheck, AMissingFileIsNotFound) {
    const auto checked = check_program((dir() / "missing").string());
    ASSERT_FALSE(checked);
    EXPECT_TRUE(checked.error().not_found);
    EXPECT_EQ(checked.error().reason, std::generic_category().message(ENOENT));
}

// Any other reason the file cannot be looked at means it cannot be run, not that it is missing.
TEST_F(ProgramCheck, APathThroughAFileCannotBeExecutedRatherThanNotFound) {
    const fs::path tool = file("tool", fs::perms::owner_all);
    const auto checked = check_program((tool / "inside").string());
    ASSERT_FALSE(checked);
    EXPECT_FALSE(checked.error().not_found);
    EXPECT_EQ(checked.error().reason, std::generic_category().message(ENOTDIR));
}

TEST_F(ProgramCheck, ADirectoryIsRefused) {
    const auto checked = check_program(dir().string());
    ASSERT_FALSE(checked);
    EXPECT_FALSE(checked.error().not_found);
    EXPECT_EQ(checked.error().reason, "not a regular file");
}

// Root too: execute access to a file needs at least one execute bit, whoever asks.
TEST_F(ProgramCheck, AFileWithNoExecutePermissionIsRefused) {
    const fs::path tool = file("tool", fs::perms::owner_read | fs::perms::owner_write);
    const auto checked = check_program(tool.string());
    ASSERT_FALSE(checked);
    EXPECT_FALSE(checked.error().not_found);
    EXPECT_EQ(checked.error().reason, std::generic_category().message(EACCES));
}

} // namespace
