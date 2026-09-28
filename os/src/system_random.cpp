#include "os/system_random.hpp"

#include <sys/random.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <unistd.h>

namespace os {

namespace {

// Assembled on the stack and sent with a single write(2): the process is about to abort, so
// nothing here may allocate, and the line should not interleave with other threads' output.
[[noreturn]] void die(int err) noexcept {
    const char* name = ::strerrorname_np(err);
    const std::string_view reason = name != nullptr ? std::string_view(name) : "unknown errno";
    std::array<char, 64> line{};
    std::size_t used = 0;
    for (const std::string_view piece :
         {std::string_view("fatal: getrandom failed: "), reason, std::string_view("\n")}) {
        used += piece.copy(std::span(line).subspan(used).data(), line.size() - used);
    }
    // If stderr is gone too there is nothing left to report to.
    [[maybe_unused]] const ssize_t written = ::write(STDERR_FILENO, line.data(), used);
    std::abort();
}

} // namespace

void SystemRandom::fill(std::span<std::byte> out) noexcept {
    std::size_t done = 0;
    while (done < out.size()) {
        const std::span<std::byte> rest = out.subspan(done);
        const ssize_t n = ::getrandom(rest.data(), rest.size(), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            // EFAULT and EINVAL are programmer errors; ENOSYS and EPERM mean a seccomp filter
            // denies the call. Ids built from a partially filled buffer would collide, so none
            // of them can be survived.
            die(errno);
        }
        done += static_cast<std::size_t>(n);
    }
}

} // namespace os
