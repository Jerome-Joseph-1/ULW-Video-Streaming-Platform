#include "pipe.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace live {

namespace {
// How often a writer waiting for room looks at its stop token.
constexpr int kWriteWaitMs = 100;
} // namespace

std::optional<Pipe> make_pipe() {
    std::array<int, 2> fds{};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    return Pipe{.read = os::UniqueFd(fds[0]), .write = os::UniqueFd(fds[1])};
}

bool write_all(int fd, std::span<const std::byte> bytes, const std::stop_token& stop) {
    while (!bytes.empty()) {
        pollfd waiting{.fd = fd, .events = POLLOUT, .revents = 0};
        if (::poll(&waiting, 1, kWriteWaitMs) < 0 && errno != EINTR) {
            return false;
        }
        if (stop.stop_requested()) {
            return false;
        }
        if ((waiting.revents & POLLOUT) == 0) {
            continue;
        }
        const ssize_t n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
    return true;
}

} // namespace live
