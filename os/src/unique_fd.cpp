#include "os/unique_fd.hpp"

#include <unistd.h>
#include <utility>

namespace os {

void UniqueFd::reset(int fd) noexcept {
    if (fd == fd_) {
        return;
    }
    const int old = std::exchange(fd_, fd);
    if (old >= 0) {
        // Linux releases the descriptor even when close() fails with EINTR, so retrying
        // would risk closing a number another thread has just been handed.
        static_cast<void>(::close(old));
    }
}

} // namespace os
