#include "os/limits.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <cerrno>

namespace os {

std::expected<NofileLimits, int> raise_nofile_limit(std::size_t cap) noexcept {
    rlimit lim{};
    if (::getrlimit(RLIMIT_NOFILE, &lim) != 0) {
        return std::unexpected(errno);
    }
    const rlim_t target = std::min<rlim_t>(lim.rlim_max, static_cast<rlim_t>(cap));
    if (lim.rlim_cur != target) {
        lim.rlim_cur = target;
        if (::setrlimit(RLIMIT_NOFILE, &lim) != 0) {
            return std::unexpected(errno);
        }
    }
    return NofileLimits{.soft = static_cast<std::size_t>(lim.rlim_cur),
                        .hard = static_cast<std::size_t>(lim.rlim_max)};
}

} // namespace os
