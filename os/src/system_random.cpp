#include "os/system_random.hpp"

#include <sys/random.h>

#include <cerrno>
#include <cstdlib>

namespace os {

void SystemRandom::fill(std::span<std::byte> out) noexcept {
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = ::getrandom(out.data() + done, out.size() - done, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            // Only EFAULT/EINVAL remain, both programmer errors; ids built from a
            // partially filled buffer would collide, so refuse to continue.
            std::abort();
        }
        done += static_cast<std::size_t>(n);
    }
}

} // namespace os
