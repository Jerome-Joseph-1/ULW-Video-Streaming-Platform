#include "infra/packagers/process_packagers.hpp"

#include "os/unique_fd.hpp"

#include "later.hpp"

#include <sys/syscall.h>
#include <sys/wait.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <fcntl.h>
#include <map>
#include <memory>
#include <spawn.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace infra::packagers {

namespace {

using core::ports::PackagerError;
using core::ports::PackagerState;

// Finished children are remembered so a sweep can tell how their stream went; past this many
// the oldest finished ones are forgotten (a stream ends long before a sweep is that far behind).
constexpr std::size_t kRemembered = 256;

class SpawnAttributes {
public:
    SpawnAttributes() noexcept : ok_(::posix_spawnattr_init(&attr_) == 0) {}
    ~SpawnAttributes() {
        if (ok_) {
            ::posix_spawnattr_destroy(&attr_);
        }
    }
    SpawnAttributes(const SpawnAttributes&) = delete;
    SpawnAttributes& operator=(const SpawnAttributes&) = delete;

    // The child starts with no signal blocked and every one this process changed back at its
    // default, in a session of its own: the gateway blocks its shutdown signals and ignores
    // SIGPIPE, and a terminal's ^C must not reach a stream.
    [[nodiscard]] bool prepare() noexcept {
        sigset_t none;
        sigset_t defaults;
        sigemptyset(&none);
        sigfillset(&defaults);
        // posix_spawn refuses to touch these two, and nothing here changed them.
        sigdelset(&defaults, SIGKILL);
        sigdelset(&defaults, SIGSTOP);
        return ok_ && ::posix_spawnattr_setsigmask(&attr_, &none) == 0 &&
               ::posix_spawnattr_setsigdefault(&attr_, &defaults) == 0 &&
               ::posix_spawnattr_setflags(&attr_, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                                      POSIX_SPAWN_SETSID) == 0;
    }

    [[nodiscard]] const posix_spawnattr_t* get() const noexcept { return &attr_; }

private:
    posix_spawnattr_t attr_{};
    bool ok_ = false;
};

class FileActions {
public:
    FileActions() noexcept : ok_(::posix_spawn_file_actions_init(&actions_) == 0) {}
    ~FileActions() {
        if (ok_) {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    }
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;

    // Nothing to read on stdin; stdout and stderr are this process's log; every other
    // descriptor this process holds (client sockets, the database's) stays here.
    [[nodiscard]] bool prepare() noexcept {
        return ok_ &&
               ::posix_spawn_file_actions_addopen(&actions_, STDIN_FILENO, "/dev/null", O_RDONLY,
                                                  0) == 0 &&
               ::posix_spawn_file_actions_addclosefrom_np(&actions_, STDERR_FILENO + 1) == 0;
    }

    [[nodiscard]] const posix_spawn_file_actions_t* get() const noexcept { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
    bool ok_ = false;
};

} // namespace

class ProcessPackagers::Impl {
public:
    Impl(net::IReactor& reactor, ProcessConfig config) noexcept
        : reactor_(reactor), config_(std::move(config)), later_(reactor) {}
    ~Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void start(const core::ports::PackagerSpec& spec, core::ports::PackagerDone done) {
        const std::string stream = spec.stream.to_string();
        if (children_.contains(stream)) {
            later_.post([done = std::move(done)]() mutable noexcept { done({}); });
            return;
        }
        auto child = spawn(spec, stream);
        if (!child) {
            later_.post([done = std::move(done), error = child.error()]() mutable noexcept {
                done(std::unexpected(error));
            });
            return;
        }
        forget_finished();
        children_.emplace(stream, std::move(*child));
        later_.post([done = std::move(done)]() mutable noexcept { done({}); });
    }

    void state(const core::LiveStreamId& stream, core::ports::PackagerStateDone done) {
        const auto found = children_.find(stream.to_string());
        const PackagerState state =
            found == children_.end() ? PackagerState::Absent : found->second->state();
        later_.post([done = std::move(done), state]() mutable noexcept { done(state); });
    }

private:
    class Child final : public net::IReadyHandler {
    public:
        Child(net::IReactor& reactor, pid_t pid, os::UniqueFd pidfd) noexcept
            : reactor_(reactor), pid_(pid), pidfd_(std::move(pidfd)) {}
        ~Child() override { stop_watching(); }
        Child(const Child&) = delete;
        Child& operator=(const Child&) = delete;

        [[nodiscard]] bool watch() noexcept {
            watching_ = reactor_.watch(pidfd_.get(), net::Interest::Read, *this).has_value();
            return watching_;
        }

        void stop_watching() noexcept {
            if (watching_) {
                reactor_.unwatch(pidfd_.get());
                watching_ = false;
            }
        }

        void on_ready(net::Interest /*ready*/) noexcept override {
            siginfo_t info{};
            if (::waitid(P_PIDFD, static_cast<id_t>(pidfd_.get()), &info, WEXITED | WNOHANG) != 0 ||
                info.si_pid == 0) {
                return;
            }
            // A packager exits 0 once its stream has ended and its recording is settled, and
            // non-zero for a restart to finish what failed; here nothing restarts it.
            state_ = info.si_code == CLD_EXITED && info.si_status == 0 ? PackagerState::Finished
                                                                       : PackagerState::Failed;
            stop_watching();
            pidfd_.reset();
        }

        [[nodiscard]] PackagerState state() const noexcept { return state_; }
        [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    private:
        net::IReactor& reactor_;
        pid_t pid_;
        os::UniqueFd pidfd_;
        // A process that runs is listening: the packager binds before anything else, in the
        // first milliseconds, and the relay's SRT caller retries its handshake for seconds.
        PackagerState state_ = PackagerState::Ready;
        bool watching_ = false;
    };

    [[nodiscard]] std::expected<std::unique_ptr<Child>, PackagerError>
    spawn(const core::ports::PackagerSpec& spec, std::string_view stream) {
        std::vector<std::string> env = config_.environment;
        env.push_back("ULW_STREAM_ID=" + std::string(stream));
        env.push_back("ULW_STREAM_OWNER=" + std::string(spec.owner.view()));
        env.push_back("ULW_LIVE_SRT_PASSPHRASE=" + spec.passphrase);
        std::vector<char*> envp;
        envp.reserve(env.size() + 1);
        for (std::string& line : env) {
            envp.push_back(line.data());
        }
        envp.push_back(nullptr);
        std::string program = config_.binary;
        std::vector<char*> argv{program.data(), nullptr};

        SpawnAttributes attributes;
        FileActions actions;
        if (!attributes.prepare() || !actions.prepare()) {
            return std::unexpected(PackagerError::Unavailable);
        }
        pid_t pid = -1;
        const int rc = ::posix_spawn(&pid, program.c_str(), actions.get(), attributes.get(),
                                     argv.data(), envp.data());
        if (rc != 0) {
            // A binary that is not there or not executable stays so.
            return std::unexpected(rc == ENOENT || rc == EACCES || rc == ENOEXEC
                                       ? PackagerError::Refused
                                       : PackagerError::Unavailable);
        }
        // Through syscall(): this glibc's <sys/pidfd.h> declares pidfd_open without C linkage.
        os::UniqueFd pidfd{static_cast<int>(::syscall(SYS_pidfd_open, pid, 0))};
        auto child = std::make_unique<Child>(reactor_, pid, std::move(pidfd));
        if (!child->watch()) {
            // Unwatched, its exit would never be seen and its zombie never reaped: stop it now.
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            return std::unexpected(PackagerError::Unavailable);
        }
        return child;
    }

    void forget_finished() {
        std::size_t finished = 0;
        for (const auto& [stream, child] : children_) {
            finished += child->state() == PackagerState::Ready ? 0U : 1U;
        }
        for (auto it = children_.begin(); it != children_.end() && finished > kRemembered;) {
            if (it->second->state() != PackagerState::Ready) {
                it = children_.erase(it);
                --finished;
            } else {
                ++it;
            }
        }
    }

    net::IReactor& reactor_;
    ProcessConfig config_;
    std::map<std::string, std::unique_ptr<Child>, std::less<>> children_;
    detail::Later later_;
};

std::expected<std::unique_ptr<ProcessPackagers>, std::string>
ProcessPackagers::create(net::IReactor& reactor, ProcessConfig config) {
    if (config.binary.empty() || config.binary.front() != '/') {
        return std::unexpected("the packager binary must be an absolute path");
    }
    if (::access(config.binary.c_str(), X_OK) != 0) {
        return std::unexpected("the packager binary is not executable: " +
                               std::generic_category().message(errno));
    }
    return std::make_unique<ProcessPackagers>(std::make_unique<Impl>(reactor, std::move(config)));
}

ProcessPackagers::ProcessPackagers(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ProcessPackagers::~ProcessPackagers() = default;

void ProcessPackagers::start(const core::ports::PackagerSpec& spec,
                             core::ports::PackagerDone done) {
    impl_->start(spec, std::move(done));
}

void ProcessPackagers::state(const core::LiveStreamId& stream,
                             core::ports::PackagerStateDone done) {
    impl_->state(stream, std::move(done));
}

} // namespace infra::packagers
