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

} // namespace

std::expected<void, std::string> make_private_dir(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return std::unexpected(std::string("empty directory name"));
    }
    std::filesystem::path at;
    for (const std::filesystem::path& part : dir) {
        at /= part;
        if (::mkdir(at.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
            return refuse_errno(at, errno);
        }
    }
    const UniqueFd fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        const int err = errno;
        if (err == ELOOP || err == ENOTDIR) {
            return refuse(dir, "not a directory (a symbolic link or a file is in its place)");
        }
        return refuse_errno(dir, err);
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return refuse_errno(dir, errno);
    }
    if (st.st_uid != ::geteuid()) {
        return refuse(dir, "owned by uid " + std::to_string(st.st_uid) +
                               ", not by this process's user (uid " + std::to_string(::geteuid()) +
                               ")");
    }
    if ((st.st_mode & 07777) != S_IRWXU && ::fchmod(fd.get(), S_IRWXU) != 0) {
        return refuse_errno(dir, errno);
    }
    return {};
}

} // namespace os
