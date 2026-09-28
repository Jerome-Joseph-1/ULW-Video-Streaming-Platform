#include "devtoken/key_file.hpp"

#include "os/unique_fd.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace devtoken {

std::expected<std::string, int> read_key_file(const std::string& path) {
    const os::UniqueFd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!fd) {
        return std::unexpected(errno);
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return std::unexpected(errno);
    }
    // As ssh does with a private key: one that others can read has already leaked.
    if (!S_ISREG(st.st_mode) || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return std::unexpected(EPERM);
    }
    std::string contents(kMaxKeyFileBytes + 1, '\0');
    std::size_t filled = 0;
    while (filled < contents.size()) {
        const ssize_t n = ::read(fd.get(), contents.data() + filled, contents.size() - filled);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return std::unexpected(errno);
        }
        if (n == 0) {
            break;
        }
        filled += static_cast<std::size_t>(n);
    }
    if (filled > kMaxKeyFileBytes) {
        return std::unexpected(EFBIG);
    }
    contents.resize(filled);
    return contents;
}

std::expected<void, int> write_new_key_file(const std::string& path, std::string_view contents) {
    // O_EXCL: an existing key is never replaced, since tokens and key sets made from it would
    // silently stop verifying. The umask can only take bits away from 0600.
    const os::UniqueFd fd{
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (!fd) {
        return std::unexpected(errno);
    }
    std::size_t written = 0;
    while (written < contents.size()) {
        const ssize_t n = ::write(fd.get(), contents.data() + written, contents.size() - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            const int err = errno;
            // A torn key file would block the next keygen and load as nothing.
            ::unlink(path.c_str());
            return std::unexpected(err);
        }
        written += static_cast<std::size_t>(n);
    }
    return {};
}

} // namespace devtoken
