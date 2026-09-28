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
#include "log.hpp"
#include "worker.hpp"
#include "workspace.hpp"

#include <sys/prctl.h>

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
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>

namespace {

namespace fs = std::filesystem;

std::optional<std::string> read_env(std::string_view name) {
    // Read once, before any thread exists, so nothing can race it with setenv.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

int fail(std::string_view what, std::string_view why) {
    std::println(stderr, "transcode_worker: {}: {}", what, why);
    return EXIT_FAILURE;
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
// handler runs in the middle of a libpq or libcurl call.
void watch_signals(const std::stop_token& stop, std::stop_source& shutdown, sigset_t signals) {
    // Wakes this often only to notice that the worker finished on its own.
    constexpr timespec kTick{.tv_sec = 0, .tv_nsec = 500'000'000};
    while (!stop.stop_requested()) {
        const int sig = ::sigtimedwait(&signals, nullptr, &kTick);
        if (sig > 0) {
            worker::log("signal {}: finishing or releasing the current job, then exiting", sig);
            shutdown.request_stop();
            return;
        }
    }
}

int run() {
    const auto info = core::build_info();
    auto config = worker::load_config(read_env);
    if (!config) {
        return fail(config.error().variable, config.error().reason);
    }
    // Makes /proc/<pid>/environ, which holds the database password and storage keys,
    // unreadable to other processes of our user, the sandboxed children included.
    if (::prctl(PR_SET_DUMPABLE, 0) != 0) {
        return fail("prctl", std::generic_category().message(errno));
    }
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    if (const int rc = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr); rc != 0) {
        return fail("block signals", std::generic_category().message(rc));
    }

    std::error_code ec;
    fs::create_directories(config->scratch, ec);
    if (ec) {
        return fail("ULW_SCRATCH_DIR", ec.message());
    }
    if (const std::size_t swept = worker::sweep_workspaces(config->scratch); swept > 0) {
        worker::log("removed {} workspaces left by an earlier run", swept);
    }
    fs::path sandbox = config->sandbox;
    if (sandbox.empty()) {
        sandbox = fs::read_symlink("/proc/self/exe", ec).parent_path() / "ulw_sandbox";
        if (ec) {
            return fail("ULW_SANDBOX_BIN", "not set, and /proc/self/exe is unreadable");
        }
    }
    const os::SystemClock clock;
    os::SystemRandom random;
    // ADR-0025: without the sandbox the worker does not start, rather than run ffmpeg bare.
    if (const auto refused = infra::ffmpeg::check_sandbox(sandbox, config->scratch, clock)) {
        return fail("sandbox", *refused);
    }
    auto storage = make_storage(*config, clock, random);
    if (!storage) {
        return fail("storage", storage.error());
    }
    infra::postgres::PgJobQueue queue(config->database_url);
    infra::postgres::PgJobQueue lease_queue(config->database_url);
    infra::ffmpeg::FfmpegTranscoder transcoder({.sandbox = sandbox,
                                                .ffmpeg = config->ffmpeg,
                                                .ffprobe = config->ffprobe,
                                                .search_path = config->search_path,
                                                .threads = config->ffmpeg_threads},
                                               clock);
    worker::JobRunner runner({.queue = queue,
                              .lease_queue = lease_queue,
                              .store = *storage->transfer,
                              .transcoder = transcoder,
                              .clock = clock,
                              .random = random,
                              .free_space = worker::free_space},
                             {.scratch = config->scratch, .node = config->node, .lease = {}});

    std::stop_source shutdown;
    const std::jthread signal_thread([&shutdown, signals](const std::stop_token& stop) {
        watch_signals(stop, shutdown, signals);
    });
    worker::log("{} ({}) node={} storage={} scratch={} threads={} sandbox={}", info.version,
                info.git_sha, config->node.view(), to_string(config->storage),
                config->scratch.string(), config->ffmpeg_threads, sandbox.string());
    worker::run_worker(queue, runner, config->node, shutdown.get_token());
    worker::log("stopped");
    return EXIT_SUCCESS;
}

} // namespace

// Formatting and allocation are all that can still throw; report it and exit.
int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        static_cast<void>(std::fputs(e.what(), stderr));
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}
