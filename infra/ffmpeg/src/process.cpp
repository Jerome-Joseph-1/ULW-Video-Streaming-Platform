#include "process.hpp"

#include "os/unique_fd.hpp"

#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <stop_token>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace infra::ffmpeg {

namespace {

// ffmpeg's warnings and errors: the tail explains a failure, and 4 KiB holds the last dozen
// lines of it.
constexpr std::size_t kStderrTail = 4096;
constexpr std::size_t kReadChunk = 16384;

std::string errno_text(int error) {
    return std::generic_category().message(error);
}

class FileActions {
public:
    FileActions() : ok_(::posix_spawn_file_actions_init(&actions_) == 0) {}
    ~FileActions() {
        if (ok_) {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    }
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;
    FileActions(FileActions&&) = delete;
    FileActions& operator=(FileActions&&) = delete;

    [[nodiscard]] bool dup2(int fd, int target) {
        ok_ = ok_ && ::posix_spawn_file_actions_adddup2(&actions_, fd, target) == 0;
        return ok_;
    }
    [[nodiscard]] const posix_spawn_file_actions_t* get() const noexcept { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
    bool ok_ = false;
};

class SpawnAttributes {
public:
    SpawnAttributes() : ok_(::posix_spawnattr_init(&attr_) == 0) {}
    ~SpawnAttributes() {
        if (ok_) {
            ::posix_spawnattr_destroy(&attr_);
        }
    }
    SpawnAttributes(const SpawnAttributes&) = delete;
    SpawnAttributes& operator=(const SpawnAttributes&) = delete;
    SpawnAttributes(SpawnAttributes&&) = delete;
    SpawnAttributes& operator=(SpawnAttributes&&) = delete;

    // The worker blocks SIGTERM in every thread to take it synchronously; a child inheriting
    // that mask could not be terminated. Its own process group keeps a terminal's ^C for us.
    [[nodiscard]] bool isolate() {
        sigset_t none;
        sigset_t all;
        sigemptyset(&none);
        sigfillset(&all);
        const auto flags = static_cast<short>(POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                              POSIX_SPAWN_SETPGROUP);
        ok_ = ok_ && ::posix_spawnattr_setsigmask(&attr_, &none) == 0 &&
              ::posix_spawnattr_setsigdefault(&attr_, &all) == 0 &&
              ::posix_spawnattr_setpgroup(&attr_, 0) == 0 &&
              ::posix_spawnattr_setflags(&attr_, flags) == 0;
        return ok_;
    }
    [[nodiscard]] const posix_spawnattr_t* get() const noexcept { return &attr_; }

private:
    posix_spawnattr_t attr_{};
    bool ok_ = false;
};

struct Pipe {
    os::UniqueFd read;
    os::UniqueFd write;
};

std::optional<Pipe> make_pipe() {
    std::array<int, 2> fds{};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    return Pipe{.read = os::UniqueFd(fds[0]), .write = os::UniqueFd(fds[1])};
}

std::vector<char*> c_strings(std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (std::string& s : strings) {
        out.push_back(s.data());
    }
    out.push_back(nullptr);
    return out;
}

std::vector<std::string> helper_argv(const Sandbox& sandbox, const Limits& limits,
                                     const Args& args) {
    std::vector<std::string> argv{sandbox.helper.string(),
                                  "--writable",
                                  limits.writable.string(),
                                  "--address-space",
                                  std::to_string(limits.address_space_bytes),
                                  "--cpu-seconds",
                                  std::to_string(limits.cpu.count()),
                                  "--"};
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
}

// The child and its process group. It may have exited already; the group id stays ours
// until we reap it, so the signal cannot reach a stranger.
void signal_group(pid_t pid, int sig) noexcept {
    static_cast<void>(::kill(-pid, sig));
}

// Reads what is there; false at end of file or on an error.
bool drain(int fd, const std::function<void(std::string_view)>& sink) {
    std::array<char, kReadChunk> buffer{};
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        return true;
    }
    if (n <= 0) {
        return false;
    }
    sink(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
    return true;
}

int poll_timeout(core::MonoTime now, core::MonoTime next) {
    const auto left = std::chrono::ceil<core::Millis>(next - now);
    return static_cast<int>(std::clamp<core::Millis::rep>(left.count(), 0, 60'000));
}

// Everything the child is connected to, opened before it starts.
struct Channels {
    os::UniqueFd devnull;
    Pipe out;
    Pipe err;
    // Written when the caller's stop token fires, so a poll on it wakes.
    os::UniqueFd wake;
};

std::expected<Channels, std::string> open_channels() {
    os::UniqueFd devnull(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    auto out = make_pipe();
    auto err = make_pipe();
    os::UniqueFd wake(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!devnull || !out || !err || !wake) {
        return std::unexpected("pipe setup: " + errno_text(errno));
    }
    return Channels{.devnull = std::move(devnull),
                    .out = std::move(*out),
                    .err = std::move(*err),
                    .wake = std::move(wake)};
}

std::expected<pid_t, std::string> spawn(const Sandbox& sandbox, const Limits& limits,
                                        const Args& args, Channels& channels) {
    FileActions actions;
    SpawnAttributes attributes;
    if (!actions.dup2(channels.devnull.get(), STDIN_FILENO) ||
        !actions.dup2(channels.out.write.get(), STDOUT_FILENO) ||
        !actions.dup2(channels.err.write.get(), STDERR_FILENO) || !attributes.isolate()) {
        return std::unexpected("spawn attributes refused");
    }
    std::vector<std::string> argv_text = helper_argv(sandbox, limits, args);
    std::vector<std::string> env_text = sandbox.environment;
    const std::vector<char*> argv = c_strings(argv_text);
    const std::vector<char*> envp = c_strings(env_text);
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, sandbox.helper.c_str(), actions.get(), attributes.get(),
                                      argv.data(), envp.data());
    if (spawned != 0) {
        return std::unexpected("spawn " + sandbox.helper.string() + ": " + errno_text(spawned));
    }
    // Only the child's copies may stay open, or we would never see end of file.
    channels.out.write.reset();
    channels.err.write.reset();
    return pid;
}

// Watches one running child until both its outputs end: hands stdout on, keeps the tail of
// stderr, and ends the child at the deadline or on request, SIGTERM first and SIGKILL after
// the grace period.
class Supervisor {
public:
    Supervisor(pid_t pid, const core::ports::IClock& clock, core::MonoTime deadline,
               ChildExit& result) noexcept
        : pid_(pid), clock_(clock), deadline_(deadline), result_(result) {}

    void run(const Channels& channels, const std::function<void(std::string_view)>& on_stdout) {
        const auto keep_tail = [this](std::string_view bytes) {
            result_.stderr_bytes += bytes.size();
            result_.stderr_tail.append(bytes);
            if (result_.stderr_tail.size() > 2 * kStderrTail) {
                result_.stderr_tail.erase(0, result_.stderr_tail.size() - kStderrTail);
            }
        };
        bool out_open = true;
        bool err_open = true;
        bool wake_armed = true;
        while (out_open || err_open) {
            const core::MonoTime now = clock_.now();
            if (now >= deadline_) {
                terminate(Ending::TimedOut, now);
            }
            if (kill_at_ && now >= *kill_at_) {
                signal_group(pid_, SIGKILL);
                kill_at_ = now + kTerminationGrace;
            }
            std::array<pollfd, 3> fds{
                pollfd{
                    .fd = out_open ? channels.out.read.get() : -1, .events = POLLIN, .revents = 0},
                pollfd{
                    .fd = err_open ? channels.err.read.get() : -1, .events = POLLIN, .revents = 0},
                pollfd{
                    .fd = wake_armed ? channels.wake.get() : -1, .events = POLLIN, .revents = 0}};
            if (::poll(fds.data(), fds.size(), poll_timeout(now, kill_at_.value_or(deadline_))) <
                    0 &&
                errno != EINTR) {
                // Nothing sensible is left to wait on; make sure the child goes.
                signal_group(pid_, SIGKILL);
                return;
            }
            if (fds[0].revents != 0) {
                out_open = drain(channels.out.read.get(), on_stdout);
            }
            if (fds[1].revents != 0) {
                err_open = drain(channels.err.read.get(), keep_tail);
            }
            if (fds[2].revents != 0) {
                wake_armed = false;
                terminate(Ending::Stopped, clock_.now());
            }
        }
    }

private:
    // The first reason to end the child is the one reported.
    void terminate(Ending why, core::MonoTime now) noexcept {
        if (kill_at_) {
            return;
        }
        result_.ending = why;
        signal_group(pid_, SIGTERM);
        kill_at_ = now + kTerminationGrace;
    }

    pid_t pid_;
    const core::ports::IClock& clock_;
    core::MonoTime deadline_;
    ChildExit& result_;
    std::optional<core::MonoTime> kill_at_;
};

} // namespace

std::expected<ChildExit, std::string>
run_sandboxed(const Sandbox& sandbox, const Limits& limits, const Args& args,
              const core::ports::IClock& clock,
              const std::function<void(std::string_view)>& on_stdout, const std::stop_token& stop) {
    auto channels = open_channels();
    if (!channels) {
        return std::unexpected(std::move(channels.error()));
    }
    const core::MonoTime started = clock.now();
    const auto pid = spawn(sandbox, limits, args, *channels);
    if (!pid) {
        return std::unexpected(pid.error());
    }
    const int wake = channels->wake.get();
    const std::stop_callback on_stop(stop, [wake]() noexcept {
        const std::uint64_t one = 1;
        // Only fails once the counter is saturated, and then it is readable anyway.
        [[maybe_unused]] const ssize_t n = ::write(wake, &one, sizeof one);
    });

    ChildExit result;
    Supervisor(*pid, clock, started + limits.wall, result).run(*channels, on_stdout);

    int status = 0;
    rusage usage{};
    while (::wait4(*pid, &status, 0, &usage) < 0 && errno == EINTR) {
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    result.wall = std::chrono::duration_cast<core::Millis>(clock.now() - started);
    result.peak_rss_kib = static_cast<std::uint64_t>(usage.ru_maxrss);
    if (result.stderr_tail.size() > kStderrTail) {
        result.stderr_tail.erase(0, result.stderr_tail.size() - kStderrTail);
    }
    return result;
}

} // namespace infra::ffmpeg
