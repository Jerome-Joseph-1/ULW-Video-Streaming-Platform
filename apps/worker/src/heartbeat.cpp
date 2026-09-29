#include "heartbeat.hpp"

#include "os/unique_fd.hpp"

#include <sys/stat.h>

#include <fcntl.h>

namespace worker {

void Heartbeat::beat() const noexcept {
    // O_NOFOLLOW: the scratch directory also holds every job's workspace, and a link left in
    // it must not turn this into a write somewhere else.
    constexpr int kFlags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW;
    constexpr mode_t kMode = 0600;
    const os::UniqueFd fd(::open(file_.c_str(), kFlags, kMode));
    if (fd) {
        static_cast<void>(::futimens(fd.get(), nullptr));
    }
}

} // namespace worker
