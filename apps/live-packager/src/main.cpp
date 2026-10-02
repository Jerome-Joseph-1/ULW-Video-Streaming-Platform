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
#include "ops/process.hpp"
#include "publisher.hpp"
#include "recorder.hpp"
#include "stream_runner.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
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

// Empties this stream's scratch directory and makes its media directory, or says why not.
[[nodiscard]] std::expected<void, std::string> clear_scratch(const fs::path& scratch) {
    // The scratch root is the deployment's to make, owned by the packager's user and 0700. Made
    // here, it would be made by a process that cannot write to /var/cache, or in a directory
    // where someone else could have made it first.
    std::error_code ec;
    const fs::path root = scratch.parent_path();
    if (!fs::is_directory(root, ec)) {
        // Missing is the usual case and gets the instructions; anything else (EACCES) is told.
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return std::unexpected(root.string() + ": " + ec.message());
        }
        return std::unexpected(root.string() +
                               " is not a directory: make it, owned by the user the packager "
                               "runs as with mode 0700, or name another");
    }
    // What an earlier run left is not needed: the store has the stream's state.
    fs::remove_all(scratch, ec);
    if (ec) {
        return std::unexpected(scratch.string() + ": " + ec.message());
    }
    // One level at a time: a root that went away since the check fails here, rather than being
    // made again by the packager.
    fs::create_directory(scratch, ec);
    if (!ec) {
        fs::create_directory(scratch / "media", ec);
    }
    if (ec) {
        return std::unexpected(scratch.string() + ": " + ec.message());
    }
    return {};
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

class PgCatalog final : public live::IRecordingCatalog {
public:
    explicit PgCatalog(std::string conninfo) : db_(std::move(conninfo)) {}

    std::expected<std::optional<infra::postgres::RecordingRow>,
                  infra::postgres::RecordingStoreError>
    find(std::string_view stream) override {
        return db_.find(stream);
    }
    std::expected<infra::postgres::RecordingRow, infra::postgres::RecordingStoreError>
    record(const infra::postgres::NewRecording& recording) override {
        return db_.record(recording);
    }
    std::expected<infra::postgres::RecordingRow, infra::postgres::RecordingStoreError>
    fail(std::string_view stream, std::string_view reason) override {
        return db_.fail(stream, reason);
    }

private:
    infra::postgres::PgLiveRecordings db_;
};

class SandboxedCopier final : public live::IRecordingCopier {
public:
    SandboxedCopier(infra::ffmpeg::RecordingRemuxConfig config, const core::ports::IClock& clock)
        : remuxer_(std::move(config), clock) {}

    std::expected<void, infra::ffmpeg::RemuxError>
    run(const infra::ffmpeg::RecordingRemuxJob& job,
        const std::function<void(std::string_view)>& on_output,
        const std::stop_token& stop) override {
        return remuxer_.run(job, on_output, stop);
    }
    std::expected<std::optional<infra::ffmpeg::AudioFormat>, infra::ffmpeg::RemuxError>
    probe_audio(const fs::path& init, const fs::path& work_dir,
                const std::stop_token& stop) override {
        return remuxer_.probe_audio(init, work_dir, stop);
    }

private:
    const infra::ffmpeg::RecordingRemuxer remuxer_;
};

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

// One line for how the recording went; only what may pass exits non-zero, for a restart to
// try again.
int report(const live::RecordResult& done) {
    switch (done.outcome) {
    case live::RecordOutcome::Recorded:
        live::log("recording: queued as video {}",
                  done.video ? done.video->to_string() : std::string());
        return EXIT_SUCCESS;
    case live::RecordOutcome::AlreadyRecorded:
        if (done.video) {
            live::log("recording: already video {}", done.video->to_string());
        } else {
            live::log("recording: already marked unrecordable: {}", done.detail);
        }
        return EXIT_SUCCESS;
    case live::RecordOutcome::NothingToRecord:
        live::log("recording: nothing to record");
        return EXIT_SUCCESS;
    case live::RecordOutcome::Superseded:
    case live::RecordOutcome::Unrecordable:
        live::log("recording: {}: {}", live::to_string(done.outcome), done.detail);
        return EXIT_SUCCESS;
    case live::RecordOutcome::Failed:
        live::log("recording: failed: {}", done.detail);
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
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
    // First, before the configuration and its secrets are read: see ops::disable_core_dumps.
    // It also makes /proc/<pid>/environ, which holds the storage keys, unreadable to other
    // processes of our user, the sandboxed ffmpeg included.
    if (auto r = ops::disable_core_dumps(); !r) {
        return fail("disable core dumps", std::generic_category().message(r.error()));
    }
    auto config = live::load_config(read_env);
    if (!config) {
        return fail(config.error().variable, config.error().reason);
    }
    // A write to a pipe whose reader is gone is an error to handle, not a signal.
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGUSR1);
    if (const int rc = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr); rc != 0) {
        return fail("block signals", std::generic_category().message(rc));
    }

    if (const auto cleared = clear_scratch(config->scratch); !cleared) {
        return fail("ULW_SCRATCH_DIR", cleared.error());
    }
    const fs::path media_dir = config->scratch / "media";
    std::error_code ec;
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
    std::optional<PgCatalog> catalog;
    std::optional<live::RecorderSettings> recorder;
    if (const std::optional<live::RecordingTarget>& target = config->recording; target) {
        catalog.emplace(target->database_url);
        recorder.emplace(live::RecorderSettings{
            .stream = config->stream,
            .owner = target->owner,
            .work_dir = config->scratch / "recording",
            .wall = config->max_duration,
            .max_bytes = live::recording_bound(config->max_kbps, config->max_duration),
            .own_claim = std::nullopt});
    }
    SandboxedCopier copier({.sandbox = sandbox,
                            .ffmpeg = config->ffmpeg,
                            .ffprobe = config->ffprobe,
                            .search_path = config->search_path},
                           clock);
    // A stream that has ended is recorded, never streamed again; the recording is repeated by
    // every run that finds it not yet done, so one killed before the job was queued is made
    // good by the next.
    const auto record = [&]() -> int {
        if (!recorder || !catalog) {
            return EXIT_SUCCESS;
        }
        const auto done = live::record_stream({.store = *storage->transfer,
                                               .streams = *storage->streams,
                                               .copier = copier,
                                               .catalog = *catalog,
                                               .clock = clock,
                                               .random = random},
                                              *recorder, drain.get_token());
        return report(done);
    };

    std::optional<std::uint32_t> claimed;
    auto publisher = live::Publisher::open({.stream = config->stream,
                                            .window = {.target_seconds = config->segment_seconds,
                                                       .max_segments = config->window_segments},
                                            .media_dir = media_dir,
                                            .outbox = config->scratch / "outbox"},
                                           *storage->transfer, clock, &claimed);
    if (!publisher && publisher.error() == live::PublishError::AlreadyEnded && recorder) {
        live::log("{} ({}) stream={} has ended; recording it", info.version, info.git_sha,
                  config->stream.str());
        recorder->own_claim = claimed;
        return record();
    }
    if (!publisher) {
        return fail("stream", live::to_string(publisher.error()));
    }
    // A run that ends the stream without publishing a segment of its own ends it above the
    // last segment's epoch; its own claim must not fence its recording out.
    if (recorder) {
        recorder->own_claim = publisher->epoch();
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
