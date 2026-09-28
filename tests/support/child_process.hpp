#pragma once

#include "os/unique_fd.hpp"

#include <sys/types.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ulw::test {

// A long-running program under test, such as a server or a worker: started with exactly the
// given environment, its stdout and stderr collected, and killed if the test leaves it running.
class ChildProcess {
public:
    // argv[0] is a path. nullptr when it could not be started.
    [[nodiscard]] static std::unique_ptr<ChildProcess> start(const std::vector<std::string>& argv,
                                                             const std::vector<std::string>& env);

    ChildProcess(pid_t pid, os::UniqueFd output, os::UniqueFd exited) noexcept;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    // Everything the program has written so far.
    [[nodiscard]] const std::string& output() const noexcept { return output_; }

    // Collects output until `text` appears in it, anywhere since the start, or `limit` passes.
    [[nodiscard]] bool wait_for_output(std::string_view text, std::chrono::milliseconds limit);
    void signal(int sig) const noexcept;
    // The exit code, 128 + the signal for a signalled program, or nullopt after `limit`.
    [[nodiscard]] std::optional<int> wait_exit(std::chrono::milliseconds limit);

private:
    // Reads what is there, waiting at most `timeout`; false once the output has ended.
    bool read_some(std::chrono::milliseconds timeout);

    pid_t pid_;
    os::UniqueFd output_fd_;
    // A pidfd: readable once the program has exited.
    os::UniqueFd exited_;
    std::string output_;
    std::optional<int> exit_code_;
};

} // namespace ulw::test
