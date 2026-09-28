#include "support/fake_random.hpp"
#include "support/temp_dir.hpp"
#include "workspace.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>

namespace {

namespace fs = std::filesystem;
using worker::Workspace;
using worker::WorkspaceError;

class WorkspaceTest : public ::testing::Test {
protected:
    ulw::test::TempDir root{"ulw-workspace"};
    ulw::test::FakeRandom random;
};

TEST_F(WorkspaceTest, IsAPrivateDirectoryOfItsOwnUnderTheRoot) {
    const auto ws = Workspace::create(root.path(), 1, random);
    ASSERT_TRUE(ws);
    EXPECT_EQ(ws->dir().parent_path(), root.path());
    EXPECT_TRUE(ws->dir().filename().string().starts_with("job-"));
    EXPECT_EQ(fs::status(ws->dir()).permissions(), fs::perms::owner_all);
    EXPECT_EQ(ws->source().parent_path(), ws->dir());
    EXPECT_EQ(ws->output().parent_path(), ws->dir());
}

TEST_F(WorkspaceTest, TakesEverythingInItWhenItGoes) {
    fs::path dir;
    {
        const auto ws = Workspace::create(root.path(), 1, random);
        ASSERT_TRUE(ws);
        dir = ws->dir();
        fs::create_directories(ws->output() / "720p");
        std::ofstream(ws->source()) << "raw";
        std::ofstream(ws->output() / "720p" / "seg_00000.m4s") << "segment";
    }
    EXPECT_FALSE(fs::exists(dir));
    EXPECT_TRUE(fs::is_empty(root.path()));
}

TEST_F(WorkspaceTest, RefusesASourceThatNeedsMoreThanAThirdOfTheFreeSpace) {
    const auto free = fs::space(root.path()).available;
    EXPECT_EQ(Workspace::create(root.path(), free / 2, random).error(),
              WorkspaceError::InsufficientSpace);
    EXPECT_TRUE(fs::is_empty(root.path()));
    EXPECT_TRUE(Workspace::create(root.path(), free / 16, random));
}

TEST_F(WorkspaceTest, AMovedFromWorkspaceRemovesNothing) {
    fs::path dir;
    std::optional<Workspace> kept;
    {
        auto ws = Workspace::create(root.path(), 1, random);
        ASSERT_TRUE(ws);
        dir = ws->dir();
        kept.emplace(std::move(*ws));
    }
    EXPECT_TRUE(fs::exists(dir));
    kept.reset();
    EXPECT_FALSE(fs::exists(dir));
}

TEST_F(WorkspaceTest, NeverSharesAnExistingDirectory) {
    ulw::test::FakeRandom same_seed_a(9);
    ulw::test::FakeRandom same_seed_b(9);
    const auto first = Workspace::create(root.path(), 1, same_seed_a);
    ASSERT_TRUE(first);
    EXPECT_EQ(Workspace::create(root.path(), 1, same_seed_b).error(), WorkspaceError::Unavailable);
    EXPECT_TRUE(fs::exists(first->dir()));
}

TEST_F(WorkspaceTest, SweepRemovesLeftoverWorkspacesAndNothingElse) {
    fs::create_directories(root.path() / "job-0123456789abcdef" / "hls");
    fs::create_directories(root.path() / "job-fedcba9876543210");
    fs::create_directories(root.path() / "keep-me");
    std::ofstream(root.path() / "job-notes.txt") << "a file, not a workspace";
    EXPECT_EQ(worker::sweep_workspaces(root.path()), 2U);
    EXPECT_TRUE(fs::exists(root.path() / "keep-me"));
    EXPECT_TRUE(fs::exists(root.path() / "job-notes.txt"));
    EXPECT_FALSE(fs::exists(root.path() / "job-0123456789abcdef"));
}

TEST_F(WorkspaceTest, AMissingRootIsCreated) {
    const auto ws = Workspace::create(root.path() / "nested" / "scratch", 1, random);
    ASSERT_TRUE(ws);
    EXPECT_TRUE(fs::exists(ws->dir()));
}

} // namespace
