#include "os/private_dir.hpp"

#include "support/temp_dir.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <climits>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

::mode_t mode_of(const fs::path& p) {
    struct stat st {};
    EXPECT_EQ(::lstat(p.c_str(), &st), 0) << p;
    return st.st_mode & 07777;
}

std::string reason(int err) {
    return std::generic_category().message(err);
}

// Runs in `dir` while it lives, and goes back to where it was.
class InDirectory {
public:
    explicit InDirectory(const fs::path& dir) : saved_(fs::current_path()) {
        fs::current_path(dir);
    }
    ~InDirectory() {
        std::error_code ec;
        fs::current_path(saved_, ec);
    }
    InDirectory(const InDirectory&) = delete;
    InDirectory& operator=(const InDirectory&) = delete;
    InDirectory(InDirectory&&) = delete;
    InDirectory& operator=(InDirectory&&) = delete;

private:
    fs::path saved_;
};

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

// A directory of the host's, owned by a user who is neither this process's nor root, that this
// process may open: a service's own under /var (man's cache, postgres's data root, a spool).
// Empty when the host has none.
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

TEST_F(MakePrivateDir, RefusesAParentAnotherUserOwns) {
    // Root makes one; anyone else looks for one of the host's. Nothing is made in it either way.
    fs::path parent;
    if (::geteuid() == 0) {
        parent = tmp.path() / "theirs";
        ASSERT_EQ(::mkdir(parent.c_str(), 0700), 0);
        ASSERT_EQ(::chmod(parent.c_str(), 0777), 0);
        ASSERT_EQ(::chown(parent.c_str(), 65534, 65534), 0);
    } else {
        parent = someone_elses_directory();
        if (parent.empty()) {
            GTEST_SKIP() << "not root, and no directory of another user's to try instead";
        }
    }
    struct stat st {};
    ASSERT_EQ(::lstat(parent.c_str(), &st), 0) << parent;
    const auto made = os::make_private_dir(parent / "ulw-private-node-1");
    ASSERT_FALSE(made);
    EXPECT_EQ(made.error(), parent.string() + ": owned by uid " + std::to_string(st.st_uid) +
                                ", neither this process's user nor root, who alone may hold the "
                                "directory it keeps its own in");
    EXPECT_FALSE(fs::exists(parent / "ulw-private-node-1"));
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

TEST_F(MakePrivateDir, RefusingAMissingParentMakesNothing) {
    const fs::path root = tmp.path() / "scratch";
    const auto made = os::make_private_dir(root / "node-1", os::MissingParent::Refuse);
    ASSERT_FALSE(made);
    EXPECT_EQ(made.error(), root.string() + ": " + reason(ENOENT));
    EXPECT_FALSE(fs::exists(root));
}

TEST_F(MakePrivateDir, RefusingAMissingParentStillMakesTheDirectoryInAnExistingOne) {
    const fs::path dir = tmp.path() / "node-1";
    const ::mode_t saved = ::umask(0);
    const auto made = os::make_private_dir(dir, os::MissingParent::Refuse);
    ::umask(saved);
    ASSERT_TRUE(made) << made.error();
    EXPECT_EQ(mode_of(dir), 0700U);
}

TEST_F(MakePrivateDir, CheckPrivateParentAcceptsADirectoryOfOurOwn) {
    const auto held = os::check_private_parent(tmp.path());
    EXPECT_TRUE(held) << held.error();
}

TEST_F(MakePrivateDir, CheckPrivateParentRefusesASymbolicLink) {
    const fs::path root = tmp.path() / "scratch";
    fs::create_directory_symlink(tmp.path(), root);
    const auto held = os::check_private_parent(root);
    ASSERT_FALSE(held);
    EXPECT_EQ(held.error(),
              root.string() + ": not a directory (a symbolic link or a file is in its place)");
}

TEST_F(MakePrivateDir, CheckPrivateParentRefusesADirectoryAnotherUserOwns) {
    fs::path parent;
    if (::geteuid() == 0) {
        parent = tmp.path() / "theirs";
        ASSERT_EQ(::mkdir(parent.c_str(), 0700), 0);
        ASSERT_EQ(::chown(parent.c_str(), 65534, 65534), 0);
    } else {
        parent = someone_elses_directory();
        if (parent.empty()) {
            GTEST_SKIP() << "not root, and no directory of another user's to try instead";
        }
    }
    struct stat st {};
    ASSERT_EQ(::lstat(parent.c_str(), &st), 0) << parent;
    const auto held = os::check_private_parent(parent);
    ASSERT_FALSE(held);
    EXPECT_EQ(held.error(), parent.string() + ": owned by uid " + std::to_string(st.st_uid) +
                                ", neither this process's user nor root, who alone may hold the "
                                "directory it keeps its own in");
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

TEST_F(MakePrivateDir, MakesABareNameInTheWorkingDirectory) {
    const InDirectory in(tmp.path());
    const auto made = os::make_private_dir("node-1");
    ASSERT_TRUE(made) << made.error();
    EXPECT_EQ(mode_of(tmp.path() / "node-1"), 0700U);
}

TEST_F(MakePrivateDir, AcceptsADirectoryKeptInOneRootOwns) {
    // The temporary directory is root's on any host this runs on; the test's own directory,
    // made in it, is the one to take.
    struct stat st {};
    if (::lstat(tmp.path().parent_path().c_str(), &st) != 0 || st.st_uid != 0) {
        GTEST_SKIP() << tmp.path().parent_path() << " is not root's";
    }
    ASSERT_EQ(::chmod(tmp.path().c_str(), 0755), 0);
    const auto made = os::make_private_dir(tmp.path());
    ASSERT_TRUE(made) << made.error();
    EXPECT_EQ(mode_of(tmp.path()), 0700U);
}

TEST_F(MakePrivateDir, AcceptsARootOwnedParentAnyoneMayWriteWithoutTheStickyBit) {
    // What a kubelet makes for an emptyDir: root's, 0777, no sticky bit. Only root can make one.
    if (::geteuid() != 0) {
        GTEST_SKIP() << "needs root";
    }
    const fs::path empty_dir = tmp.path() / "empty-dir";
    ASSERT_EQ(::mkdir(empty_dir.c_str(), 0700), 0);
    ASSERT_EQ(::chmod(empty_dir.c_str(), 0777), 0);
    const auto made = os::make_private_dir(empty_dir / "node-1");
    ASSERT_TRUE(made) << made.error();
    EXPECT_EQ(mode_of(empty_dir / "node-1"), 0700U);
    EXPECT_EQ(mode_of(empty_dir), 0777U);
}

TEST_F(MakePrivateDir, NamesTheComponentAboveThatCannotBeMade) {
    // A file where a directory above it should be: mkdir finds something there and moves on,
    // and the component below it cannot be made.
    std::ofstream(tmp.path() / "file") << "x";
    const fs::path below = tmp.path() / "file" / "scratch";
    const auto made = os::make_private_dir(below / "node-1");
    ASSERT_FALSE(made);
    EXPECT_EQ(made.error(), below.string() + ": " + reason(ENOTDIR));
}

TEST_F(MakePrivateDir, NamesTheDirectoryThatCannotBeMadeInItsParent) {
    const fs::path dir = tmp.path() / std::string(NAME_MAX + 1, 'n');
    const auto made = os::make_private_dir(dir);
    ASSERT_FALSE(made);
    EXPECT_EQ(made.error(), dir.string() + ": " + reason(ENAMETOOLONG));
    EXPECT_TRUE(fs::is_empty(tmp.path()));
}

TEST_F(MakePrivateDir, ReportsAnOpenThatFailsForAnyOtherReasonAsThatReason) {
    // A parent name no file system accepts is not a link or a file in the way, and is not told
    // as one. Nothing above is made, so the open is what fails.
    const fs::path parent = tmp.path() / std::string(NAME_MAX + 1, 'p');
    const auto made = os::make_private_dir(parent / "node-1", os::MissingParent::Refuse);
    ASSERT_FALSE(made);
    EXPECT_EQ(made.error(), parent.string() + ": " + reason(ENAMETOOLONG));
    EXPECT_TRUE(fs::is_empty(tmp.path()));
}

} // namespace
