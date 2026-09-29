#include "core/version.hpp"
#include "infra/ffmpeg/live_remux.hpp"
#include "infra/ffmpeg/recording_remux.hpp"
#include "infra/ffmpeg/transcoder.hpp"
#include "infra/postgres/live_recordings.hpp"
#include "infra/s3util/credentials.hpp"
#include "infra/s3util/profile.hpp"
#include "infra/srt/ingest.hpp"
#include "infra/storage/fs_transfer.hpp"
#include "infra/storage/s3_transfer.hpp"
#include "os/system_clock.hpp"
#include "os/system_random.hpp"

#include "config.hpp"
#include "log.hpp"
#include "publisher.hpp"
#include "recorder.hpp"
#include "stream_runner.hpp"

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
    std::println(stderr, "live_packager: {}: {}", what, why);
    return EXIT_FAILURE;
}

std::string_view to_string(live::StorageBackend backend) noexcept {
    switch (backend) {
    case live::StorageBackend::R2:
        return "r2";
    case live::StorageBackend::Minio:
        return "minio";
    case live::StorageBackend::Filesystem:
        return "fs";
    }
    return "r2";
}

struct Storage {
    std::unique_ptr<infra::s3util::EnvCredentialProvider> credentials;
    std::unique_ptr<core::ports::IObjectTransfer> transfer;
    // The same adapter as `transfer`, for the recording.
    core::ports::IObjectStreams* streams = nullptr;
};

std::expected<Storage, std::string> make_storage(const live::Config& config,
                                                 const core::ports::IClock& clock,
                                                 core::ports::IRandom& random) {
    using live::StorageBackend;
    Storage storage;
    if (config.storage == StorageBackend::Filesystem) {
        auto transfer = std::make_unique<infra::storage::FsTransfer>(config.storage_location);
        storage.streams = transfer.get();
        storage.transfer = std::move(transfer);
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
    storage.streams = transfer->get();
    storage.transfer = std::move(*transfer);
    return storage;
}

// SIGTERM, SIGINT and SIGUSR1 are blocked in every thread and taken here, synchronously, so no
// handler runs in the middle of a libcurl call. SIGTERM and SIGINT drain: the process goes and
// the stream is left to be continued. SIGUSR1 ends the stream, and a SIGTERM after it still
// stops the recording that follows.
void watch_signals(const std::stop_token& stop, std::stop_source& drain, std::stop_source& end,
                   sigset_t signals) {
    // Wakes this often only to notice that the packager finished on its own.
    constexpr timespec kTick{.tv_sec = 0, .tv_nsec = 100'000'000};
    while (!stop.stop_requested()) {
        const int sig = ::sigtimedwait(&signals, nullptr, &kTick);
        if (sig == SIGUSR1) {
            live::log("signal {}: ending the stream", sig);
            end.request_stop();
            continue;
        }
        if (sig > 0) {
            live::log("signal {}: draining, the stream is left to be continued", sig);
            drain.request_stop();
            return;
        }
    }
}

int run() {
    const auto info = core::build_info();
    auto config = live::load_config(read_env);
    if (!config) {
        return fail(config.error().variable, config.error().reason);
    }
    // A write to a pipe whose reader is gone is an error to handle, not a signal.
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
    // Makes /proc/<pid>/environ, which holds the storage keys, unreadable to other processes
    // of our user, the sandboxed ffmpeg included.
    if (::prctl(PR_SET_DUMPABLE, 0) != 0) {
        return fail("prctl", std::generic_category().message(errno));
    }
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGUSR1);
    if (const int rc = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr); rc != 0) {
        return fail("block signals", std::generic_category().message(rc));
    }

    // What an earlier run left is not needed: the store has the stream's state.
    std::error_code ec;
    fs::remove_all(config->scratch, ec);
    const fs::path media_dir = config->scratch / "media";
    fs::create_directories(media_dir, ec);
    if (ec) {
        return fail("ULW_SCRATCH_DIR", ec.message());
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
    // ADR-0025: without the sandbox the packager does not start, rather than run ffmpeg bare.
    if (const auto refused = infra::ffmpeg::check_sandbox(sandbox, media_dir, clock)) {
        return fail("sandbox", *refused);
    }
    auto storage = make_storage(*config, clock, random);
    if (!storage) {
        return fail("storage", storage.error());
    }
    std::stop_source drain;
    std::stop_source end;
    const std::jthread signal_thread([&drain, &end, signals](const std::stop_token& stop) {
        watch_signals(stop, drain, end, signals);
    });
    const infra::ffmpeg::LiveRemuxConfig ffmpeg{
        .sandbox = sandbox, .ffmpeg = config->ffmpeg, .search_path = config->search_path};
    std::optional<infra::postgres::PgLiveRecordings> recordings;
    std::optional<live::RecorderSettings> recorder;
    if (config->recording) {
        recordings.emplace(config->recording->database_url);
        recorder.emplace(live::RecorderSettings{.stream = config->stream,
                                                .owner = config->recording->owner,
                                                .work_dir = config->scratch / "recording",
                                                .budget = config->max_duration});
    }
    const infra::ffmpeg::RecordingRemuxer copier(ffmpeg, clock);
    // A stream that has ended is recorded, never streamed again; the recording is repeated by
    // every run that finds it not yet done, so one killed before the job was queued is made
    // good by the next.
    const auto record = [&]() -> int {
        if (!recorder) {
            return EXIT_SUCCESS;
        }
        const auto video = live::record_stream({.store = *storage->transfer,
                                                .streams = *storage->streams,
                                                .remuxer = copier,
                                                .recordings = *recordings,
                                                .clock = clock,
                                                .random = random},
                                               *recorder, drain.get_token());
        if (!video) {
            live::log("recording: {}", live::to_string(video.error()));
            return EXIT_FAILURE;
        }
        if (!*video) {
            live::log("recording: nothing to record");
        }
        return EXIT_SUCCESS;
    };

    auto publisher = live::Publisher::open({.stream = config->stream,
                                            .window = {.target_seconds = config->segment_seconds,
                                                       .max_segments = config->window_segments},
                                            .media_dir = media_dir,
                                            .outbox = config->scratch / "outbox"},
                                           *storage->transfer, clock);
    if (!publisher && publisher.error() == live::PublishError::AlreadyEnded && recorder) {
        live::log("{} ({}) stream={} has ended; recording it", info.version, info.git_sha,
                  config->stream.str());
        return record();
    }
    if (!publisher) {
        return fail("stream", live::to_string(publisher.error()));
    }
    // After everything that holds an SRT socket is declared: main's locals are destroyed in
    // reverse, and libsrt's cleanup must come last.
    const auto srt = infra::srt::Runtime::start();
    if (!srt) {
        return fail("srt", srt.error());
    }
    auto listener = infra::srt::IngestListener::bind(*srt, {.host = config->ingest_host,
                                                            .port = config->ingest_port,
                                                            .passphrase = config->srt_passphrase,
                                                            .stream_id = config->stream.str()});
    if (!listener) {
        return fail("ingest", listener.error());
    }
    const infra::ffmpeg::LiveRemuxer remuxer(ffmpeg, clock);
    live::log("{} ({}) stream={} storage={} ingest={}:{} segment={}s window={} sandbox={} "
              "recording={}",
              info.version, info.git_sha, config->stream.str(), to_string(config->storage),
              config->ingest_host, listener->port(), config->segment_seconds,
              config->window_segments, sandbox.string(), recorder ? "on" : "off");
    if (publisher->resumed()) {
        live::log("continuing at segment {} as epoch {}", publisher->next_sequence(),
                  publisher->epoch());
    }
    const auto outcome =
        live::run_stream(*publisher, *listener, remuxer, clock,
                         {.media_dir = media_dir,
                          .segment_seconds = config->segment_seconds,
                          .listed_segments = live::listed_segments(config->window_segments),
                          .max_kbps = config->max_kbps,
                          .max_duration = config->max_duration},
                         {.drain = drain.get_token(), .end = end.get_token()});
    live::log("stopped");
    const bool drained = drain.stop_requested() && !end.stop_requested();
    // The stored playlist says whether the stream ended, whatever this run's outcome: a run
    // that failed still ends it, and a superseded one leaves it to the newer.
    const int recorded = drained ? EXIT_SUCCESS : record();
    return outcome == live::Outcome::Ended && recorded == EXIT_SUCCESS ? EXIT_SUCCESS
                                                                       : EXIT_FAILURE;
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
