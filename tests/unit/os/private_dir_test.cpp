#include "os/private_dir.hpp"

#include "support/temp_dir.hpp"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

::mode_t mode_of(const fs::path& p) {
    struct stat st {};
    EXPECT_EQ(::lstat(p.c_str(), &st), 0) << p;
    return st.st_mode & 07777;
}

class MakePrivateDir : public ::testing::Test {
protected:
    ulw::test::TempDir tmp{"ulw-private"};
};

TEST_F(MakePrivateDir, CreatesEveryMissingComponentOwnerOnlyWhateverTheUmask) {
    const fs::path root = tmp.path() / "scratch";
    const fs::path dir = root / "node-1";
    const ::mode_t saved = ::umask(0);
    const auto made = os::make_private_dir(dir);
    ::umask(saved);
    ASSERT_TRUE(made) << made.error();
    EXPECT_EQ(mode_of(root), 0700U);
    EXPECT_EQ(mode_of(dir), 0700U);
}

TEST_F(MakePrivateDir, TightensADirectoryOfOurOwnThatOthersCouldReach) {
    const fs::path dir = tmp.path() / "node-1";
    ASSERT_EQ(::mkdir(dir.c_str(), 0700), 0);
    ASSERT_EQ(::chmod(dir.c_str(), 0777), 0);
    const auto made = os::make_private_dir(dir);
    ASSERT_TRUE(made) << made.error();
    EXPECT_EQ(mode_of(dir), 0700U);
}

TEST_F(MakePrivateDir, RefusesASymbolicLinkAndLeavesWhereItPointsAlone) {
    const fs::path target = tmp.path() / "elsewhere";
    ASSERT_EQ(::mkdir(target.c_str(), 0755), 0);
    const fs::path dir = tmp.path() / "node-1";
    fs::create_directory_symlink(target, dir);
    const auto made = os::make_private_dir(dir);
    ASSERT_FALSE(made);
    EXPECT_NE(made.error().find("not a directory"), std::string::npos) << made.error();
    EXPECT_EQ(mode_of(target), 0755U);
}

TEST_F(MakePrivateDir, RefusesAFileInTheDirectorysPlace) {
    const fs::path dir = tmp.path() / "node-1";
    std::ofstream(dir) << "x";
    const auto made = os::make_private_dir(dir);
    ASSERT_FALSE(made);
    EXPECT_NE(made.error().find("not a directory"), std::string::npos) << made.error();
}

TEST_F(MakePrivateDir, RefusesADirectoryAnotherUserOwns) {
    // Root can hand a directory to another user. Anyone else is not root, and /usr is root's
    // on any host this runs on; it is refused before anything is changed in it.
    fs::path dir = "/usr";
    if (::geteuid() != 0) {
        struct stat st {};
        if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0) {
            GTEST_SKIP() << "not root, and /usr is not a directory of root's to try instead";
        }
    } else {
        dir = tmp.path() / "theirs";
        ASSERT_EQ(::mkdir(dir.c_str(), 0777), 0);
        ASSERT_EQ(::chmod(dir.c_str(), 0777), 0);
        ASSERT_EQ(::chown(dir.c_str(), 65534, 65534), 0);
    }
    const ::mode_t before = mode_of(dir);
    const auto made = os::make_private_dir(dir);
    ASSERT_FALSE(made);
    EXPECT_NE(made.error().find("owned by uid"), std::string::npos) << made.error();
    EXPECT_EQ(mode_of(dir), before);
}

TEST_F(MakePrivateDir, RefusesAParentAnotherUserOwns) {
    // Only root can make a directory another user owns.
    if (::geteuid() != 0) {
        GTEST_SKIP() << "needs root";
    }
    const fs::path root = tmp.path() / "theirs";
    ASSERT_EQ(::mkdir(root.c_str(), 0700), 0);
    ASSERT_EQ(::chmod(root.c_str(), 0777), 0);
    ASSERT_EQ(::chown(root.c_str(), 65534, 65534), 0);
    const auto made = os::make_private_dir(root / "node-1");
    ASSERT_FALSE(made);
    EXPECT_NE(made.error().find("owned by uid 65534"), std::string::npos) << made.error();
    EXPECT_FALSE(fs::exists(root / "node-1"));
}

TEST_F(MakePrivateDir, RefusesAParentThatIsASymbolicLink) {
    const fs::path target = tmp.path() / "elsewhere";
    ASSERT_EQ(::mkdir(target.c_str(), 0755), 0);
    const fs::path root = tmp.path() / "scratch";
    fs::create_directory_symlink(target, root);
    const auto made = os::make_private_dir(root / "node-1");
    ASSERT_FALSE(made);
    EXPECT_NE(made.error().find("not a directory"), std::string::npos) << made.error();
    EXPECT_FALSE(fs::exists(target / "node-1"));
}

TEST_F(MakePrivateDir, RefusesAPathEndingInASlash) {
    // open("link/", O_DIRECTORY | O_NOFOLLOW) would follow the link.
    const fs::path target = tmp.path() / "elsewhere";
    ASSERT_EQ(::mkdir(target.c_str(), 0755), 0);
    fs::create_directory_symlink(target, tmp.path() / "node-1");
    const auto made = os::make_private_dir(tmp.path() / "node-1/");
    ASSERT_FALSE(made);
    EXPECT_EQ(mode_of(target), 0755U);
}

} // namespace
