// The helper's lookup of the program it is asked to run and its check of the path it finds,
// called here directly, and the production helper's table; the refusals as the helper reports
// them, by exit code and message, are in sandbox_test.cpp.
#include "command.hpp"
#include "program_check.hpp"
#include "sandbox_programs.hpp"
#include "support/temp_dir.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using infra::ffmpeg::sandbox::check_program;
using infra::ffmpeg::sandbox::find_program;
using infra::ffmpeg::sandbox::Program;
using infra::ffmpeg::sandbox::program_to_run;

// The table ulw_sandbox is built with: ffmpeg and ffprobe at the paths CMake was given, and
// nothing else, under the names the adapter's command lines use.
TEST(ProductionPrograms, AreExactlyFfmpegAndFfprobeAtTheirBuiltInPaths) {
    const auto& table = infra::ffmpeg::sandbox::kPrograms;
    ASSERT_EQ(table.size(), 2U);
    EXPECT_EQ(table[0].name, infra::ffmpeg::kFfmpeg);
    EXPECT_STREQ(table[0].path, ULW_SANDBOX_FFMPEG);
    EXPECT_EQ(table[1].name, infra::ffmpeg::kFfprobe);
    EXPECT_STREQ(table[1].path, ULW_SANDBOX_FFPROBE);
    for (const Program& program : table) {
        const std::filesystem::path path(program.path);
        EXPECT_TRUE(path.is_absolute()) << program.path;
        EXPECT_EQ(path, path.lexically_normal()) << program.path;
    }
}

constexpr std::array kTable{Program{.name = "ffmpeg", .path = "/opt/a/ffmpeg"},
                            Program{.name = "ffprobe", .path = "/opt/b/ffprobe"}};

TEST(FindProgram, ANameInTheTableGivesTheTablesPath) {
    EXPECT_EQ(find_program(kTable, "ffmpeg"), kTable[0].path);
    EXPECT_EQ(find_program(kTable, "ffprobe"), kTable[1].path);
}

TEST(FindProgram, ANameNotInTheTableIsNotFound) {
    for (const std::string_view name : {"sh", "", "FFMPEG", "ffmpeg ", "ffmpeg\n", "ffmpe"}) {
        const auto found = find_program(kTable, name);
        ASSERT_FALSE(found) << name;
        EXPECT_TRUE(found.error().not_found) << name;
        EXPECT_EQ(found.error().reason, "not a program this helper was built to run");
    }
    EXPECT_FALSE(find_program({}, "ffmpeg"));
}

// Even the very path the table holds: the caller never names a file.
TEST(FindProgram, APathIsRefusedRatherThanNotFound) {
    for (const std::string_view path :
         {"/opt/a/ffmpeg", "/usr/bin/ffmpeg", "bin/ffmpeg", "./ffmpeg", "ffmpeg/", "/"}) {
        const auto found = find_program(kTable, path);
        ASSERT_FALSE(found) << path;
        EXPECT_FALSE(found.error().not_found) << path;
        EXPECT_EQ(found.error().reason,
                  "a path, not a program name; only the programs built in are run");
    }
}

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

// The lookup and the check together, as the helper makes them.
TEST_F(ProgramCheck, TheProgramToRunIsTheTablesPathOnceItPassesTheCheck) {
    const std::string tool = file("tool", fs::perms::owner_all).string();
    const std::array table{Program{.name = "tool", .path = tool.c_str()}};
    const auto found = program_to_run(table, "tool");
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, tool.c_str());
}

TEST_F(ProgramCheck, TheProgramToRunIsRefusedForAnUnknownNameOrAPath) {
    const std::string tool = file("tool", fs::perms::owner_all).string();
    const std::array table{Program{.name = "tool", .path = tool.c_str()}};
    const auto unknown = program_to_run(table, "other");
    ASSERT_FALSE(unknown);
    EXPECT_TRUE(unknown.error().not_found);
    const auto path = program_to_run(table, tool);
    ASSERT_FALSE(path);
    EXPECT_FALSE(path.error().not_found);
    EXPECT_EQ(path.error().reason,
              "a path, not a program name; only the programs built in are run");
}

// A table path that fails the check is refused as check_program refuses it, naming the path.
TEST_F(ProgramCheck, ATablePathThatFailsTheCheckIsRefusedNamingIt) {
    const std::string missing = (dir() / "missing").string();
    const std::string locked = file("locked", fs::perms::owner_read).string();
    const std::array table{Program{.name = "missing", .path = missing.c_str()},
                           Program{.name = "locked", .path = locked.c_str()},
                           Program{.name = "bare", .path = "sh"}};
    const auto gone = program_to_run(table, "missing");
    ASSERT_FALSE(gone);
    EXPECT_TRUE(gone.error().not_found);
    EXPECT_EQ(gone.error().reason, missing + ": " + std::generic_category().message(ENOENT));
    const auto refused = program_to_run(table, "locked");
    ASSERT_FALSE(refused);
    EXPECT_FALSE(refused.error().not_found);
    EXPECT_EQ(refused.error().reason, locked + ": " + std::generic_category().message(EACCES));
    const auto bare = program_to_run(table, "bare");
    ASSERT_FALSE(bare);
    EXPECT_TRUE(bare.error().not_found);
    EXPECT_EQ(bare.error().reason, "sh: not a path, and no PATH is searched");
}

} // namespace
