#pragma once

#include <sys/resource.h>
#include <sys/types.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace ulw::test {

// The "Max core file size" row of /proc/<pid>/limits, as "<soft> <hard>", or nullopt when it
// cannot be read. The file is world-readable even for a process that is not dumpable.
inline std::optional<std::string> core_limit_of(pid_t pid) {
    std::ifstream limits("/proc/" + std::to_string(pid) + "/limits");
    std::string line;
    while (std::getline(limits, line)) {
        if (!line.starts_with("Max core file size")) {
            continue;
        }
        std::string soft;
        std::string hard;
        std::istringstream fields(line.substr(sizeof "Max core file size" - 1));
        fields >> soft >> hard;
        soft += ' ';
        soft += hard;
        return soft;
    }
    return std::nullopt;
}

// Raises this process's soft RLIMIT_CORE to its hard limit for as long as it lives, so a child
// started meanwhile could write a core unless it turns that off itself: most hosts start with a
// soft limit of 0, which would make a check of the child's limit pass whatever it did.
// `raised()` is false when the hard limit is 0 too, and there is nothing to show.
class RaisedCoreLimit {
public:
    RaisedCoreLimit() {
        if (::getrlimit(RLIMIT_CORE, &saved_) != 0 || saved_.rlim_max == 0) {
            return;
        }
        const rlimit raised{.rlim_cur = saved_.rlim_max, .rlim_max = saved_.rlim_max};
        raised_ = ::setrlimit(RLIMIT_CORE, &raised) == 0;
    }
    ~RaisedCoreLimit() {
        if (raised_) {
            static_cast<void>(::setrlimit(RLIMIT_CORE, &saved_));
        }
    }
    RaisedCoreLimit(const RaisedCoreLimit&) = delete;
    RaisedCoreLimit& operator=(const RaisedCoreLimit&) = delete;
    RaisedCoreLimit(RaisedCoreLimit&&) = delete;
    RaisedCoreLimit& operator=(RaisedCoreLimit&&) = delete;

    [[nodiscard]] bool raised() const noexcept { return raised_; }

private:
    rlimit saved_{};
    bool raised_ = false;
};

} // namespace ulw::test
