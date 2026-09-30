#include "os/private_dir.hpp"

#include "os/unique_fd.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace os {

namespace {

std::unexpected<std::string> refuse(const std::filesystem::path& at, std::string_view why) {
    return std::unexpected(at.string() + ": " + std::string(why));
}

std::unexpected<std::string> refuse_errno(const std::filesystem::path& at, int err) {
    return refuse(at, std::generic_category().message(err));
}

// A directory opened without following a link in its last component. Earlier components were
// resolved by path, as any path is; the one that matters is the one this names.
std::expected<UniqueFd, std::string> open_directory(int at, const std::filesystem::path& path,
                                                    const std::filesystem::path& shown) {
    UniqueFd fd(::openat(at, path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        const int err = errno;
        if (err == ELOOP || err == ENOTDIR) {
            return refuse(shown, "not a directory (a symbolic link or a file is in its place)");
        }
        return refuse_errno(shown, err);
    }
    return fd;
}

std::expected<uid_t, std::string> owner(int fd, const std::filesystem::path& shown) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return refuse_errno(shown, errno);
    }
    return st.st_uid;
}

} // namespace

std::expected<void, std::string> make_private_dir(const std::filesystem::path& dir) {
    // "link/" would be opened through the link, O_NOFOLLOW or not.
    if (!dir.has_filename()) {
        return refuse(dir, "names no directory (empty, or it ends in '/')");
    }
    const std::filesystem::path parent = dir.has_parent_path() ? dir.parent_path() : ".";
    std::filesystem::path at;
    for (const std::filesystem::path& part : parent) {
        at /= part;
        if (::mkdir(at.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
            return refuse_errno(at, errno);
        }
    }
    // Whoever owns the parent can rename our directory away and put another in its place, so it
    // must be ours or root's: a Kubernetes emptyDir, the image's own directory or a PrivateTmp
    // qualifies, one another user made first in a shared /var/tmp does not.
    auto parent_fd = open_directory(AT_FDCWD, parent, parent);
    if (!parent_fd) {
        return std::unexpected(std::move(parent_fd.error()));
    }
    const auto parent_owner = owner(parent_fd->get(), parent);
    if (!parent_owner) {
        return std::unexpected(parent_owner.error());
    }
    if (*parent_owner != ::geteuid() && *parent_owner != 0) {
        return refuse(parent, "owned by uid " + std::to_string(*parent_owner) +
                                  ", neither this process's user nor root, who alone may hold "
                                  "the directory it keeps its own in");
    }
    const std::filesystem::path name = dir.filename();
    if (::mkdirat(parent_fd->get(), name.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
        return refuse_errno(dir, errno);
    }
    auto fd = open_directory(parent_fd->get(), name, dir);
    if (!fd) {
        return std::unexpected(std::move(fd.error()));
    }
    const auto dir_owner = owner(fd->get(), dir);
    if (!dir_owner) {
        return std::unexpected(dir_owner.error());
    }
    if (*dir_owner != ::geteuid()) {
        return refuse(dir, "owned by uid " + std::to_string(*dir_owner) +
                               ", not by this process's user (uid " + std::to_string(::geteuid()) +
                               ")");
    }
    if (::fchmod(fd->get(), S_IRWXU) != 0) {
        return refuse_errno(dir, errno);
    }
    return {};
}

} // namespace os
