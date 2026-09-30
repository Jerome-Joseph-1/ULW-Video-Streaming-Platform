#include "child_process.hpp"

#include <sys/syscall.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>
#include <utility>

namespace ulw::test {

namespace {

std::vector<char*> c_strings(std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (std::string& s : strings) {
        out.push_back(s.data());
    }
    out.push_back(nullptr);
    return out;
}

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return static_cast<int>(std::max<std::chrono::milliseconds::rep>(left.count(), 0));
}

int remaining_ms_up(std::chrono::steady_clock::time_point deadline) {
    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return static_cast<int>(std::max<std::chrono::milliseconds::rep>(left.count(), 0));
}

} // namespace

std::unique_ptr<ChildProcess> ChildProcess::start(const std::vector<std::string>& argv,
                                                  const std::vector<std::string>& env) {
    std::array<int, 2> fds{};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        return nullptr;
    }
    os::UniqueFd read_end(fds[0]);
    const os::UniqueFd write_end(fds[1]);
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDERR_FILENO);
    std::vector<std::string> args = argv;
    std::vector<std::string> vars = env;
    const auto arg_ptrs = c_strings(args);
    const auto var_ptrs = c_strings(vars);
    pid_t pid = 0;
    const int spawned =
        ::posix_spawn(&pid, arg_ptrs[0], &actions, nullptr, arg_ptrs.data(), var_ptrs.data());
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        return nullptr;
    }
    os::UniqueFd exited(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
    return std::make_unique<ChildProcess>(pid, std::move(read_end), std::move(exited));
}

ChildProcess::ChildProcess(pid_t pid, os::UniqueFd output, os::UniqueFd exited) noexcept
    : pid_(pid), output_fd_(std::move(output)), exited_(std::move(exited)) {}

ChildProcess::~ChildProcess() {
    if (!exit_code_) {
        // A stopped program must be woken for the kill to be delivered and reaped.
        ::kill(pid_, SIGKILL);
        ::kill(pid_, SIGCONT);
        int status = 0;
        ::waitpid(pid_, &status, 0);
    }
}

bool ChildProcess::read_some(std::chrono::milliseconds timeout) {
    if (!output_fd_) {
        return false;
    }
    pollfd pfd{.fd = output_fd_.get(), .events = POLLIN, .revents = 0};
    if (::poll(&pfd, 1, static_cast<int>(timeout.count())) <= 0) {
        return true;
    }
    std::array<char, 4096> buffer{};
    const ssize_t n = ::read(output_fd_.get(), buffer.data(), buffer.size());
    if (n <= 0) {
        output_fd_.reset();
        return false;
    }
    output_.append(buffer.data(), static_cast<std::size_t>(n));
    return true;
}

bool ChildProcess::wait_for_output(std::string_view text, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (output_.find(text) == std::string::npos) {
        const int left = remaining_ms(deadline);
        if (left == 0 || !read_some(std::chrono::milliseconds(left))) {
            return output_.find(text) != std::string::npos;
        }
    }
    return true;
}

bool ChildProcess::poll_until(const std::function<bool()>& ready, std::chrono::milliseconds limit,
                              std::chrono::milliseconds period) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!ready()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto next = std::min(deadline, now + period);
        // Rounded up: cut to whole milliseconds, the wait for the deadline ends short of it,
        // and the check would be asked in a spin through the fraction left.
        for (int left = remaining_ms_up(next); left > 0; left = remaining_ms_up(next)) {
            if (!read_some(std::chrono::milliseconds(left))) {
                return false;
            }
        }
    }
    return true;
}

void ChildProcess::signal(int sig) const noexcept {
    ::kill(pid_, sig);
}

std::optional<int> ChildProcess::wait_exit(std::chrono::milliseconds limit) {
    if (exit_code_) {
        return exit_code_;
    }
    const auto deadline = std::chrono::steady_clock::now() + limit;
    // Keeps draining the output: a program blocked on a full pipe would never exit.
    while (true) {
        std::array<pollfd, 2> fds{
            pollfd{.fd = output_fd_ ? output_fd_.get() : -1, .events = POLLIN, .revents = 0},
            pollfd{.fd = exited_.get(), .events = POLLIN, .revents = 0}};
        if (::poll(fds.data(), fds.size(), remaining_ms(deadline)) <= 0) {
            return std::nullopt;
        }
        if (fds[1].revents != 0) {
            break;
        }
        read_some(std::chrono::milliseconds(0));
    }
    // What the program wrote just before it exited.
    while (output_fd_) {
        pollfd pfd{.fd = output_fd_.get(), .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, 0) <= 0 || !read_some(std::chrono::milliseconds(0))) {
            break;
        }
    }
    int status = 0;
    if (::waitpid(pid_, &status, 0) != pid_) {
        return std::nullopt;
    }
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return exit_code_;
}

Started start_until_listening(const std::function<std::unique_ptr<ChildProcess>()>& start,
                              std::string_view ready_marker, std::chrono::milliseconds limit) {
    Started last;
    for (int attempt = 1; attempt <= kPortAttempts; ++attempt) {
        last.process = start();
        if (last.process == nullptr) {
            return last;
        }
        last.ready = last.process->wait_for_output(ready_marker, limit);
        if (last.ready ||
            last.process->output().find("Address already in use") == std::string::npos) {
            return last;
        }
    }
    return last;
}

} // namespace ulw::test
