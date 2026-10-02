#include "core/version.hpp"
#include "infra/ffmpeg/transcoder.hpp"
#include "infra/postgres/job_queue.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/profile.hpp"
#include "infra/storage/fs_transfer.hpp"
#include "infra/storage/s3_transfer.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "config.hpp"
#include "job_runner.hpp"
#include "ops/log.hpp"
#include "ops/notify.hpp"
#include "ops/process.hpp"
#include "ops/root.hpp"
#include "ops/settings.hpp"
#include "worker.hpp"
#include "workspace.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <pthread.h>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::optional<std::string> read_env(std::string_view name) {
    // Read once, before any thread exists, so nothing can race it with setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

// What configuration errors exit with, so a supervisor can tell a deployment that will never
// start from one that crashed.
constexpr int kExitConfig = 2;

int fail(ops::Logger& log, std::string_view what, std::string_view why) {
    log.error("startup failed", {{"step", what}, {"error", why}});
    return EXIT_FAILURE;
}

int refuse(ops::Logger& log, std::string_view source, std::string_view reason) {
    log.error("configuration refused", {{"source", source}, {"reason", reason}});
    return kExitConfig;
}

// Started as root, the worker becomes the configured user before its first job, and before the
// scratch directory it will own is made; not root, there is nothing to give up. nullopt means
// carry on, anything else is the exit code.
std::optional<int> leave_root(const std::string& user, bool allow_root, ops::Logger& log) {
    const auto step = ops::leave_root(user, allow_root);
    if (!step) {
        return step.error().configuration ? refuse(log, step.error().source, step.error().reason)
                                          : fail(log, step.error().source, step.error().reason);
    }
    if (*step == ops::RootStep::StayedRoot) {
        log.warn("running as root, as ULW_ALLOW_ROOT=1 allows");
    } else if (*step == ops::RootStep::Dropped) {
        log.info("dropped root", {{"user", user}});
    }
    return std::nullopt;
}

std::string_view to_string(worker::StorageBackend backend) noexcept {
    switch (backend) {
    case worker::StorageBackend::R2:
        return "r2";
    case worker::StorageBackend::Minio:
        return "minio";
    case worker::StorageBackend::Filesystem:
        return "fs";
    }
    return "r2";
}

struct Storage {
    std::unique_ptr<infra::s3util::EnvCredentialProvider> credentials;
    std::unique_ptr<core::ports::IObjectTransfer> transfer;
};

std::expected<Storage, std::string> make_storage(const worker::Config& config,
                                                 const core::ports::IClock& clock,
                                                 core::ports::IRandom& random) {
    using worker::StorageBackend;
    Storage storage;
    if (config.storage == StorageBackend::Filesystem) {
        storage.transfer = std::make_unique<infra::storage::FsTransfer>(config.storage_location);
        return storage;
    }
    auto profile = config.storage == StorageBackend::R2
                       ? infra::s3util::S3Profile::r2(config.storage_location)
                       : infra::s3util::S3Profile::minio(config.storage_location);
    if (!profile) {
        return std::unexpected("object store location is malformed");
    }
    auto credentials =
        infra::s3util::EnvCredentialProvider::load({.access_key_id = "ULW_S3_ACCESS_KEY_ID",
                                                    .secret_access_key = "ULW_S3_SECRET_ACCESS_KEY",
                                                    .session_token = std::nullopt});
    if (!credentials) {
        return std::unexpected(credentials.error().variable + " is unset or malformed");
    }
    storage.credentials =
        std::make_unique<infra::s3util::EnvCredentialProvider>(std::move(*credentials));
    auto transfer = infra::storage::S3Transfer::create({.credentials = *storage.credentials,
                                                        .clock = clock,
                                                        .random = random,
                                                        .profile = std::move(*profile),
                                                        .bucket = config.bucket});
    if (!transfer) {
        return std::unexpected("object store configuration refused");
    }
    storage.transfer = std::move(*transfer);
    return storage;
}

// SIGTERM and SIGINT are blocked in every thread and taken here, synchronously, so no
// handler runs in the middle of a libpq or libcurl call. The service manager's watchdog is fed
// from here too: a job's own progress is guarded by its lease, and a transcode may rightly run
// for longer than any watchdog, so what this proves is that the process still schedules.
void watch_signals(const std::stop_token& stop, std::stop_source& shutdown, sigset_t signals,
                   const std::optional<ops::Notifier>& notifier, ops::Logger& log) {
    // Wakes this often to notice that the worker finished on its own; systemd's shortest
    // sensible WatchdogSec is seconds, so half a second never lets a ping come late.
    constexpr timespec kTick{.tv_sec = 0, .tv_nsec = 500'000'000};
    const auto watchdog = notifier ? notifier->watchdog_interval() : std::nullopt;
    auto next_ping = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        if (watchdog && std::chrono::steady_clock::now() >= next_ping) {
            notifier->watchdog();
            next_ping = std::chrono::steady_clock::now() + *watchdog;
        }
        const int sig = ::sigtimedwait(&signals, nullptr, &kTick);
        if (sig > 0) {
            log.info("stopping", {{"signal", sig}});
            if (notifier) {
                notifier->stopping();
            }
            shutdown.request_stop();
            return;
        }
    }
}

int run(std::span<const std::string_view> args) {
    const auto info = core::build_info();
    const os::SystemClock clock;
    ops::StdoutSink sink;
    ops::Logger log(sink, clock, "worker", ops::Level::Info);
    // First, before the configuration and its secrets are read: see ops::disable_core_dumps.
    // It also makes /proc/<pid>/environ, which holds the database password and storage keys,
    // unreadable to other processes of our user, the sandboxed children included.
    if (auto r = ops::disable_core_dumps(); !r) {
        return fail(log, "disable core dumps", std::generic_category().message(r.error()));
    }

    const auto cli = ops::parse_command_line(worker::settings(), args);
    if (!cli) {
        return refuse(log, cli.error().source, cli.error().reason);
    }
    if (cli->version) {
        std::println("transcode_worker {} ({})", info.version, info.git_sha);
        return EXIT_SUCCESS;
    }
    const auto layers = ops::load_settings(worker::settings(), *cli, read_env);
    if (!layers) {
        return refuse(log, layers.error().source, layers.error().reason);
    }
    auto config = worker::load_config(layers->lookup());
    if (!config) {
        return refuse(log, config.error().variable, config.error().reason);
    }
    log.set_threshold(config->log_level);
    log.info("starting", {{"version", info.version}, {"git_sha", info.git_sha}});
    worker::log_effective(*config, *layers, log);
    // Before any thread exists: glibc then has no other thread to carry the change to.
    if (const auto code = leave_root(config->run_as_user, config->allow_root, log)) {
        return *code;
    }
    if (cli->check) {
        log.info("configuration valid");
        return EXIT_SUCCESS;
    }
    auto notifier = ops::Notifier::from_env(read_env, ::getpid());
    if (!notifier) {
        return refuse(log, "NOTIFY_SOCKET", std::generic_category().message(notifier.error()));
    }
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    if (const int rc = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr); rc != 0) {
        return fail(log, "block signals", std::generic_category().message(rc));
    }

    std::error_code ec;
    fs::create_directories(config->scratch, ec);
    if (ec) {
        return fail(log, "ULW_SCRATCH_DIR", ec.message());
    }
    if (const std::size_t swept = worker::sweep_workspaces(config->scratch); swept > 0) {
        log.info("removed workspaces left by an earlier run", {{"count", swept}});
    }
    fs::path sandbox = config->sandbox;
    if (sandbox.empty()) {
        sandbox = fs::read_symlink("/proc/self/exe", ec).parent_path() / "ulw_sandbox";
        if (ec) {
            return fail(log, "ULW_SANDBOX_BIN", "not set, and /proc/self/exe is unreadable");
        }
    }
    os::SystemRandom random;
    // ADR-0025: without the sandbox the worker does not start, rather than run ffmpeg bare.
    if (const auto refused = infra::ffmpeg::check_sandbox(sandbox, config->scratch, clock)) {
        return fail(log, "sandbox", *refused);
    }
    auto storage = make_storage(*config, clock, random);
    if (!storage) {
        return fail(log, "storage", storage.error());
    }
    infra::postgres::PgJobQueue queue(config->database_url);
    infra::postgres::PgJobQueue lease_queue(config->database_url);
    infra::ffmpeg::FfmpegTranscoder transcoder({.sandbox = sandbox,
                                                .ffmpeg = config->ffmpeg,
                                                .ffprobe = config->ffprobe,
                                                .search_path = config->search_path,
                                                .threads = config->ffmpeg_threads},
                                               clock);
    // Beside the node's scratch directory rather than in it, which holds workspaces only; named
    // for the node, so two workers sharing a scratch root keep a heartbeat each.
    const worker::Heartbeat heartbeat(config->scratch.parent_path() /
                                      ("heartbeat-" + std::string(config->node.view())));
    worker::JobRunner runner({.queue = queue,
                              .lease_queue = lease_queue,
                              .store = *storage->transfer,
                              .transcoder = transcoder,
                              .clock = clock,
                              .random = random,
                              .free_space = worker::free_space,
                              .log = log,
                              .heartbeat = &heartbeat},
                             {.scratch = config->scratch, .node = config->node, .lease = {}});

    std::stop_source shutdown;
    const std::jthread signal_thread(
        [&shutdown, signals, &notifier, &log](const std::stop_token& stop) {
            watch_signals(stop, shutdown, signals, *notifier, log);
        });
    log.info("started", {{"node", config->node.view()},
                         {"storage", to_string(config->storage)},
                         {"scratch", config->scratch.string()},
                         {"threads", config->ffmpeg_threads},
                         {"sandbox", sandbox.string()}});
    if (const std::optional<ops::Notifier>& manager = *notifier; manager) {
        manager->ready();
    }
    worker::run_worker(queue, runner, config->node, heartbeat, log, shutdown.get_token());
    log.info("stopped");
    return EXIT_SUCCESS;
}

} // namespace

// Formatting and allocation are all that can still throw; report it and exit.
int main(int argc, char** argv) {
    try {
        const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
        std::vector<std::string_view> args;
        for (const char* a : raw.subspan(1)) {
            args.emplace_back(a);
        }
        return run(args);
    } catch (const std::exception& e) {
        static_cast<void>(std::fputs(e.what(), stderr));
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}
