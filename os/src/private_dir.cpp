#include "os/private_dir.hpp"

#include "os/unique_fd.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace os {

namespace {

std::unexpected<std::string> refuse(const std::filesystem::path& at, std::string_view why) {
    return std::unexpected(at.string() + ": " + std::string(why));
}

std::unexpected<std::string> refuse_errno(const std::filesystem::path& at, int err) {
    return refuse(at, std::generic_category().message(err));
}

// A directory opened without following a link in its last component, and the user who owns
// it. Earlier components were resolved by path, as any path is; the one that matters is the one
// this names.
struct Directory {
    UniqueFd fd;
    uid_t owner;
};

std::expected<Directory, std::string> open_directory(int at, const std::filesystem::path& path,
                                                     const std::filesystem::path& shown) {
    UniqueFd fd(::openat(at, path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        const int err = errno;
        if (err == ELOOP || err == ENOTDIR) {
            return refuse(shown, "not a directory (a symbolic link or a file is in its place)");
        }
        return refuse_errno(shown, err);
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return refuse_errno(shown, errno);
    }
    return Directory{.fd = std::move(fd), .owner = st.st_uid};
}

// The parent of a private directory, opened, or why it cannot hold one.
std::expected<Directory, std::string> open_private_parent(const std::filesystem::path& parent) {
    // A Kubernetes emptyDir, the image's own directory or a PrivateTmp qualifies, one another
    // user made first in a shared directory does not. Its mode is not checked: a root-owned
    // parent that anyone may write, without the sticky bit, is accepted, because that is what a
    // kubelet-made emptyDir is (0777, or 2777 with an fsGroup), and in a pod no other user
    // shares it.
    auto parent_dir = open_directory(AT_FDCWD, parent, parent);
    if (!parent_dir) {
        return std::unexpected(std::move(parent_dir.error()));
    }
    if (parent_dir->owner != ::geteuid() && parent_dir->owner != 0) {
        return refuse(parent, "owned by uid " + std::to_string(parent_dir->owner) +
                                  ", neither this process's user nor root, who alone may hold "
                                  "the directory it keeps its own in");
    }
    return parent_dir;
}

} // namespace

std::expected<void, std::string> check_private_parent(const std::filesystem::path& parent) {
    if (auto held = open_private_parent(parent); !held) {
        return std::unexpected(std::move(held.error()));
    }
    return {};
}

std::expected<void, std::string> make_private_dir(const std::filesystem::path& dir,
                                                  MissingParent missing) {
    // "link/" would be opened through the link, O_NOFOLLOW or not.
    if (!dir.has_filename()) {
        return refuse(dir, "names no directory (empty, or it ends in '/')");
    }
    const std::filesystem::path parent = dir.has_parent_path() ? dir.parent_path() : ".";
    if (missing == MissingParent::Make) {
        std::filesystem::path at;
        for (const std::filesystem::path& part : parent) {
            at /= part;
            if (::mkdir(at.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
                return refuse_errno(at, errno);
            }
        }
    }
    const auto parent_dir = open_private_parent(parent);
    if (!parent_dir) {
        return std::unexpected(parent_dir.error());
    }
    const std::filesystem::path name = dir.filename();
    if (::mkdirat(parent_dir->fd.get(), name.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
        return refuse_errno(dir, errno);
    }
    const auto made = open_directory(parent_dir->fd.get(), name, dir);
    if (!made) {
        return std::unexpected(made.error());
    }
    if (made->owner != ::geteuid()) {
        return refuse(dir, "owned by uid " + std::to_string(made->owner) +
                               ", not by this process's user (uid " + std::to_string(::geteuid()) +
                               ")");
    }
    if (::fchmod(made->fd.get(), S_IRWXU) != 0) {
        return refuse_errno(dir, errno);
    }
    return {};
}

} // namespace os
