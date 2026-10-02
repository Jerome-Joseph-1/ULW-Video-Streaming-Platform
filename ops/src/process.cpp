#include "ops/process.hpp"

#include "core/util/parse.hpp"
#include "os/unique_fd.hpp"

#include <sys/prctl.h>
#include <sys/resource.h>

#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <string_view>
#include <unistd.h>

namespace ops {

namespace {

struct CloseDir {
    void operator()(DIR* d) const noexcept { ::closedir(d); }
};

} // namespace

std::optional<std::uint64_t> open_descriptors() noexcept {
    const std::unique_ptr<DIR, CloseDir> dir{::opendir("/proc/self/fd")};
    if (!dir) {
        return std::nullopt;
    }
    std::uint64_t n = 0;
    // readdir() is only unsafe on a stream two threads share; this one is the call's own.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    while (const dirent* entry = ::readdir(dir.get())) {
        if (entry->d_name[0] != '.') {
            ++n;
        }
    }
    // The directory stream held one of the descriptors it listed.
    return n - 1;
}

std::optional<std::uint64_t> resident_bytes() noexcept {
    const os::UniqueFd fd{::open("/proc/self/statm", O_RDONLY | O_CLOEXEC)};
    if (!fd) {
        return std::nullopt;
    }
    // "size resident shared text lib data dt", in pages: seven numbers of at most 20 digits.
    std::array<char, 160> buf{};
    const ssize_t n = ::read(fd.get(), buf.data(), buf.size() - 1);
    if (n <= 0) {
        return std::nullopt;
    }
    std::string_view text(buf.data(), static_cast<std::size_t>(n));
    const std::size_t first = text.find(' ');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    text.remove_prefix(first + 1);
    const auto pages = core::parse_integer<std::uint64_t>(text.substr(0, text.find(' ')));
    const long page = ::sysconf(_SC_PAGESIZE);
    if (!pages || page <= 0) {
        return std::nullopt;
    }
    return *pages * static_cast<std::uint64_t>(page);
}

std::expected<void, int> disable_core_dumps() noexcept {
    const rlimit none{.rlim_cur = 0, .rlim_max = 0};
    if (::setrlimit(RLIMIT_CORE, &none) != 0) {
        return std::unexpected(errno);
    }
    if (::prctl(PR_SET_DUMPABLE, 0) != 0) {
        return std::unexpected(errno);
    }
    return {};
}

} // namespace ops
