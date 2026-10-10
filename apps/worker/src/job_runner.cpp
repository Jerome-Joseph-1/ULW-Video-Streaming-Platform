#include "job_runner.hpp"

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"

#include "workspace.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace worker {

namespace {

namespace fs = std::filesystem;
using core::ports::StorageError;
using core::ports::TranscodeError;
using core::ports::TranscodeFailure;

enum class Output : std::uint8_t { Segment, Init, Playlist };

// ffmpeg 6.1's HLS muxer exits 0 when a write fails for want of space, leaving truncated
// output that then fails verification, as bad input would. A failed write leaves free what it
// could not use, less than one segment: the largest, 4 s of 1080p at its 5350 kbit/s peak,
// is 2.7 MB. Below 16 MiB the disk is taken to have filled up under the job.
constexpr std::uint64_t kNearlyFull = std::uint64_t{16} << 20U;

const core::ContentType& content_type(Output output) {
    // Parsed once each; the literals are valid media types.
    static const auto segment = *core::ContentType::parse("video/iso.segment");
    static const auto init = *core::ContentType::parse("video/mp4");
    static const auto playlist = *core::ContentType::parse("application/vnd.apple.mpegurl");
    switch (output) {
    case Output::Segment:
        return segment;
    case Output::Init:
        return init;
    case Output::Playlist:
        return playlist;
    }
    return playlist;
}

// What a video's owner may read in error_reason; the details stay in our log.
std::string_view public_reason(TranscodeFailure failure) noexcept {
    switch (failure) {
    case TranscodeFailure::Rejected:
        return "the file could not be decoded as video";
    case TranscodeFailure::Crashed:
        return "the decoder crashed on this file";
    case TranscodeFailure::SyscallBlocked:
        return "the decoder was stopped by the sandbox";
    case TranscodeFailure::Killed:
        return "the transcoder was killed";
    case TranscodeFailure::OverBudget:
        return "transcoding exceeded its time budget";
    case TranscodeFailure::Stopped:
        return "transcoding was stopped";
    case TranscodeFailure::Sandbox:
        return "the transcoder sandbox is unavailable";
    case TranscodeFailure::Unverified:
        return "the transcoded output failed verification";
    case TranscodeFailure::Inaccessible:
        return "the transcoder could not read its working files";
    }
    return "transcoding failed";
}

std::string output_key(const core::VideoId& video, std::string_view rest) {
    return "videos/" + video.to_string() + "/hls/" + std::string(rest);
}

std::uint64_t own_peak_rss_kib() noexcept {
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    // glibc declares the field inside an anonymous union.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
}

struct Metrics {
    core::Millis media{};
    core::Millis transcode{};
    std::uint64_t ffmpeg_peak_rss_kib = 0;
    // -1 until ffmpeg has run.
    int ffmpeg_exit = -1;
};

struct RequestStop {
    std::stop_source* source;
    void operator()() const noexcept { source->request_stop(); }
};

// The job's percent as clients see it (videos-and-playback.md#progress): the encode takes it to
// kEncodedPercent, publishing the output to kPublishedPercent, and only the finish, which makes
// the video ready, completes it. A client never sees 100 while the video cannot play yet.
constexpr std::uint8_t kEncodedPercent = 90;
constexpr std::uint8_t kPublishedPercent = 99;

class KeeperProgress final : public core::ports::ITranscodeProgress {
public:
    KeeperProgress(LeaseKeeper& keeper, core::Millis duration) noexcept
        : keeper_(keeper), duration_(duration) {}

    void on_progress(core::Millis encoded) noexcept override {
        const auto percent = std::clamp<core::Millis::rep>(
            encoded.count() * kEncodedPercent / std::max<core::Millis::rep>(duration_.count(), 1),
            0, kEncodedPercent);
        keeper_.report(static_cast<std::uint8_t>(percent));
    }

private:
    LeaseKeeper& keeper_;
    core::Millis duration_;
};

} // namespace

Disposition disposition(TranscodeFailure failure, bool reran) noexcept {
    switch (failure) {
    case TranscodeFailure::Crashed:
    case TranscodeFailure::SyscallBlocked:
        return reran ? Disposition::FailPermanently : Disposition::RerunOnce;
    case TranscodeFailure::Killed:
    case TranscodeFailure::Sandbox:
    case TranscodeFailure::Inaccessible:
        return Disposition::Requeue;
    case TranscodeFailure::Stopped:
        return Disposition::Abandon;
    case TranscodeFailure::Rejected:
    case TranscodeFailure::OverBudget:
    case TranscodeFailure::Unverified:
        return Disposition::FailPermanently;
    }
    return Disposition::FailPermanently;
}

std::string_view to_string(core::ports::JobQueueError error) noexcept {
    switch (error) {
    case core::ports::JobQueueError::Unavailable:
        return "unavailable";
    case core::ports::JobQueueError::Corrupt:
        return "corrupt";
    case core::ports::JobQueueError::Invalid:
        return "invalid";
    }
    return "unavailable";
}

std::string_view to_string(JobOutcome outcome) noexcept {
    switch (outcome) {
    case JobOutcome::Done:
        return "done";
    case JobOutcome::Failed:
        return "failed";
    case JobOutcome::Requeued:
        return "requeued";
    case JobOutcome::Abandoned:
        return "abandoned";
    case JobOutcome::FencedOut:
        return "fenced-out";
    case JobOutcome::Unrecorded:
        return "unrecorded";
    }
    return "unknown";
}

class JobRunner::Attempt {
public:
    Attempt(const JobDeps& deps, const JobSettings& settings, const core::ports::ClaimedJob& job,
            const std::stop_token& shutdown)
        : deps_(deps), settings_(settings), job_(job),
          keeper_(deps.lease_queue, deps.log, settings.node, job.lease, settings.lease, abandon_,
                  deps.heartbeat),
          on_shutdown_(shutdown, RequestStop{&abandon_}) {}

    JobOutcome execute() {
        const auto size = deps_.store.size(job_.source);
        if (!size) {
            return storage_failure("source size", size.error());
        }
        auto workspace = Workspace::create(settings_.scratch, *size, deps_.random);
        if (!workspace) {
            const bool full = workspace.error() == WorkspaceError::InsufficientSpace;
            log().warn(
                "workspace refused",
                {{"job", id()}, {"reason", full ? "not enough free space" : "scratch unusable"}});
            return fail("no scratch space for the source", /*retryable=*/true);
        }
        if (auto got = deps_.store.download(job_.source, workspace->source()); !got) {
            return storage_failure("download", got.error());
        }
        if (abandoned()) {
            return stop_outcome();
        }

        const auto media = with_rerun(
            [&] { return deps_.transcoder.probe(workspace->source(), abandon_.get_token()); });
        if (!media) {
            return transcode_failure("probe", media.error(), *workspace);
        }
        metrics_.media = media->duration;
        const auto ladder = core::choose_ladder(media->height);
        log().info("probed", {{"job", id()},
                              {"width", media->width},
                              {"height", media->height},
                              {"fps_num", media->frame_rate.num},
                              {"fps_den", media->frame_rate.den},
                              {"duration_ms", media->duration.count()},
                              {"audio", media->has_audio},
                              {"rungs", ladder.size()}});
        if (ladder.empty()) {
            return fail("the video is too small to encode", /*retryable=*/false);
        }
        // The source is on disk by now, so what is free has to hold the output alone.
        const std::uint64_t needed = output_bytes(media->duration, ladder, media->has_audio);
        if (const auto available = deps_.free_space(workspace->dir());
            !available || *available < needed) {
            log().warn("workspace too small for the output",
                       {{"job", id()}, {"free_bytes", available.value_or(0)}, {"needed", needed}});
            return fail("no scratch space for the output", /*retryable=*/true);
        }

        KeeperProgress progress(keeper_, media->duration);
        const auto stats = with_rerun([&] {
            std::error_code ec;
            // A rerun starts from an empty directory, not a crashed run's leftovers.
            fs::remove_all(workspace->output(), ec);
            return deps_.transcoder.run(workspace->source(), workspace->output(), *media, ladder,
                                        progress, abandon_.get_token());
        });
        if (!stats) {
            metrics_.ffmpeg_exit = stats.error().exit_code;
            return transcode_failure("transcode", stats.error(), *workspace);
        }
        metrics_.ffmpeg_exit = 0;
        metrics_.transcode = stats->wall;
        metrics_.ffmpeg_peak_rss_kib = stats->peak_rss_kib;

        const auto verified = with_rerun([&] {
            return deps_.transcoder.verify(workspace->output(), *media, ladder,
                                           abandon_.get_token());
        });
        if (!verified) {
            return transcode_failure("verify", verified.error(), *workspace);
        }
        if (abandoned()) {
            return stop_outcome();
        }
        // The last cheap chance to find out we are a zombie before writing any object.
        if (const auto held = deps_.queue.heartbeat(job_.lease, settings_.node); held && !*held) {
            log().warn("fenced out", {{"job", id()}, {"call", "heartbeat before publishing"}});
            return JobOutcome::FencedOut;
        }
        auto renditions = publish(workspace->output(), ladder, media->has_audio);
        if (!renditions && keeper_.lost()) {
            log().warn("publish stopped", {{"job", id()}, {"error", renditions.error()}});
            return JobOutcome::Abandoned;
        }
        if (!renditions) {
            log().warn("publish failed", {{"job", id()}, {"error", renditions.error()}});
            return fail("publishing the output failed", /*retryable=*/true);
        }
        const auto finished = deps_.queue.finish(job_.lease, media->duration, *renditions);
        if (!finished) {
            queue_failed("finish", finished.error());
            // The database refused the result itself, and would refuse it again after every
            // rerun the lease lapsing would bring; the job fails now instead of three
            // transcodes later.
            if (finished.error() == core::ports::JobQueueError::Invalid) {
                return fail("the transcoded result could not be recorded", /*retryable=*/false);
            }
            return JobOutcome::Unrecorded;
        }
        if (!*finished) {
            log().warn("fenced out", {{"job", id()}, {"call", "finish"}});
            return JobOutcome::FencedOut;
        }
        return JobOutcome::Done;
    }

    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }

private:
    // The worker has no metrics endpoint; its counters are log lines, one per event, that carry
    // the running total for whatever tails them.
    void count_blocked(TranscodeFailure kind) {
        static std::atomic<std::uint64_t> total{0};
        if (kind == TranscodeFailure::SyscallBlocked) {
            log().warn("ffmpeg syscall blocked",
                       {{"job", id()}, {"ffmpeg_syscall_blocked_total", ++total}});
        }
    }

    [[nodiscard]] std::int64_t id() const noexcept { return std::to_underlying(job_.lease.job); }
    [[nodiscard]] ops::Logger& log() const noexcept { return deps_.log; }

    // An outage is a warning, retried by the lease; a refusal is a bug, and an error.
    void queue_failed(std::string_view call, core::ports::JobQueueError error) {
        log().log(
            error == core::ports::JobQueueError::Unavailable ? ops::Level::Warn : ops::Level::Error,
            "job queue call failed", {{"job", id()}, {"call", call}, {"error", to_string(error)}});
    }
    [[nodiscard]] bool abandoned() const noexcept { return abandon_.stop_requested(); }

    // Runs `step` again once when its first failure calls for it.
    template <class Step> std::invoke_result_t<Step&> with_rerun(Step step) {
        auto result = step();
        if (!result) {
            count_blocked(result.error().kind);
        }
        if (!result && disposition(result.error().kind, false) == Disposition::RerunOnce) {
            log().warn("step rerun", {{"job", id()}, {"error", result.error().detail}});
            result = step();
            rerun_ = true;
            if (!result) {
                count_blocked(result.error().kind);
            }
        }
        return result;
    }

    JobOutcome transcode_failure(std::string_view step, const TranscodeError& error,
                                 const Workspace& workspace) {
        log().warn("step failed", {{"job", id()}, {"step", step}, {"error", error.detail}});
        if (error.kind == TranscodeFailure::Inaccessible) {
            // The host's fault, not the upload's: every job on this worker will hit it until an
            // operator fixes the scratch directory's permissions or mounts.
            static std::atomic<std::uint64_t> total{0};
            log().error("transcoder refused its own files",
                        {{"job", id()},
                         {"step", step},
                         {"worker_files_refused_total", ++total},
                         {"error", error.detail}});
        }
        if (error.kind != TranscodeFailure::Stopped) {
            if (const auto left = deps_.free_space(workspace.dir()); left && *left < kNearlyFull) {
                log().warn("scratch disk filled up", {{"job", id()}, {"free_bytes", *left}});
                return fail("scratch space ran out", /*retryable=*/true);
            }
        }
        switch (disposition(error.kind, rerun_)) {
        case Disposition::Abandon:
            return stop_outcome();
        case Disposition::Requeue:
            return fail(public_reason(error.kind), /*retryable=*/true);
        case Disposition::RerunOnce:
        case Disposition::FailPermanently:
            return fail(public_reason(error.kind), /*retryable=*/false);
        }
        return fail(public_reason(error.kind), /*retryable=*/false);
    }

    JobOutcome storage_failure(std::string_view step, StorageError error) {
        log().warn("storage failed",
                   {{"job", id()}, {"step", step}, {"error", core::ports::to_string(error)}});
        if (error == StorageError::NotFound) {
            return fail("the uploaded file is missing", /*retryable=*/false);
        }
        return fail("object storage unavailable", /*retryable=*/true);
    }

    // Either the lease is gone, and nothing more may be written, or we are shutting down, and
    // the job goes back to the queue now instead of when its lease lapses.
    JobOutcome stop_outcome() {
        if (keeper_.lost()) {
            return JobOutcome::Abandoned;
        }
        return fail("worker stopped", /*retryable=*/true);
    }

    JobOutcome fail(std::string_view reason, bool retryable) {
        const auto written = deps_.queue.fail(job_.lease, reason, retryable);
        if (!written) {
            // Nothing is left to try: whatever the cause, the lease lapses and the reaper
            // requeues the job or fails it.
            queue_failed("fail", written.error());
            return JobOutcome::Unrecorded;
        }
        if (!*written) {
            log().warn("fenced out", {{"job", id()}, {"call", "fail"}});
            return JobOutcome::FencedOut;
        }
        return retryable ? JobOutcome::Requeued : JobOutcome::Failed;
    }

    std::expected<void, std::string> upload(const fs::path& file, const std::string& key,
                                            Output kind) {
        // The new owner may be publishing the same keys; the finish would be fenced out, but
        // every object written after the loss could overwrite one of theirs.
        if (keeper_.lost()) {
            return std::unexpected("lease lost before " + key);
        }
        // ffmpeg ran on hostile input with write access to this tree; a link it left could
        // name any file the worker can read, such as its own /proc/self/environ.
        std::error_code ec;
        const auto status = fs::symlink_status(file, ec);
        if (!fs::is_regular_file(status)) {
            return std::unexpected("not a regular file: " + file.filename().string());
        }
        const auto parsed = core::StorageKey::parse(key);
        if (!parsed) {
            return std::unexpected("unaddressable output " + key);
        }
        const std::uint64_t bytes = fs::file_size(file, ec);
        if (auto r = deps_.store.upload(file, *parsed, content_type(kind)); !r) {
            return std::unexpected(key + ": " + std::string(core::ports::to_string(r.error())));
        }
        published_bytes_.fetch_add(ec ? 0 : bytes);
        const std::size_t done = published_.fetch_add(1) + 1;
        constexpr auto kShare = static_cast<std::size_t>(kPublishedPercent - kEncodedPercent);
        const std::size_t share =
            std::min(done, to_publish_) * kShare / std::max<std::size_t>(to_publish_, 1);
        keeper_.raise(static_cast<std::uint8_t>(kEncodedPercent + share));
        return {};
    }

    struct PublishItem {
        fs::path file;
        std::string key;
        Output kind = Output::Segment;
    };

    // A rung's init and media segments, in name order, to `items`.
    std::expected<void, std::string> list_segments(const fs::path& dir, const std::string& rung,
                                                   std::vector<PublishItem>& items) const {
        std::vector<fs::path> files;
        std::error_code ec;
        if (!fs::is_directory(fs::symlink_status(dir, ec))) {
            return std::unexpected(rung + ": not a directory");
        }
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            files.push_back(entry.path());
        }
        if (ec) {
            return std::unexpected(rung + ": " + ec.message());
        }
        std::ranges::sort(files);
        for (const fs::path& file : files) {
            const std::string name = file.filename().string();
            if (name == "index.m3u8") {
                continue;
            }
            std::optional<Output> kind;
            if (file.extension() == ".m4s") {
                kind = Output::Segment;
            } else if (file.extension() == ".mp4") {
                kind = Output::Init;
            } else {
                return std::unexpected("unexpected output " + name);
            }
            items.push_back({.file = file,
                             .key = output_key(job_.video, std::format("{}/{}", rung, name)),
                             .kind = *kind});
        }
        return {};
    }

    // Uploads `items` from up to `threads` threads at once, this one among them, each taking
    // the next item not yet taken (ADR-0102). The first failure stops every thread from taking
    // another; one losing the lease fails its next upload, so the pool stops within one request
    // per thread. With one thread it is the sequential loop, in order.
    std::expected<void, std::string> upload_all(std::span<const PublishItem> items,
                                                std::size_t threads) {
        std::atomic<std::size_t> next{0};
        std::atomic<bool> failed{false};
        std::mutex mutex;
        std::optional<std::string> first_error;
        const auto drain = [&] {
            while (!failed.load()) {
                const std::size_t i = next.fetch_add(1);
                if (i >= items.size()) {
                    return;
                }
                if (auto r = upload(items[i].file, items[i].key, items[i].kind); !r) {
                    const std::scoped_lock lock(mutex);
                    if (!first_error) {
                        first_error = std::move(r.error());
                    }
                    failed.store(true);
                    return;
                }
            }
        };
        {
            std::vector<std::jthread> pool;
            pool.reserve(threads > 0 ? threads - 1 : 0);
            for (std::size_t t = 1; t < threads; ++t) {
                try {
                    pool.emplace_back(drain);
                } catch (const std::system_error& e) {
                    // Fewer threads publish more slowly, not wrongly.
                    log().warn("publish thread not started", {{"job", id()}, {"error", e.what()}});
                    break;
                }
            }
            drain();
        }
        if (first_error) {
            return std::unexpected(std::move(*first_error));
        }
        return {};
    }

    // Segments, then media playlists, then the master: a reader who finds a playlist finds
    // everything it names, and the master, written last, is the commit point. Segments go up
    // from a pool of settings_.publish_concurrency threads; the playlists, a handful, one by
    // one once every segment is up.
    std::expected<std::vector<core::ports::Rendition>, std::string>
    publish(const fs::path& out, std::span<const core::Rung> ladder, bool has_audio) {
        std::vector<PublishItem> segments;
        for (const core::Rung& rung : ladder) {
            if (auto r = list_segments(out / rung.name, rung.name, segments); !r) {
                return std::unexpected(r.error());
            }
        }
        // Every segment, each rung's playlist and the master.
        to_publish_ = segments.size() + ladder.size() + 1;
        published_.store(0);
        published_bytes_.store(0);
        const std::size_t threads = std::max<std::size_t>(
            1, std::min<std::size_t>(settings_.publish_concurrency, segments.size()));
        log().info("publishing", {{"job", id()}, {"files", to_publish_}, {"concurrency", threads}});
        const auto started = deps_.clock.now();
        keeper_.raise(kEncodedPercent);
        if (auto r = upload_all(segments, threads); !r) {
            return std::unexpected(r.error());
        }
        std::vector<core::ports::Rendition> renditions;
        for (const core::Rung& rung : ladder) {
            const std::string key = output_key(job_.video, rung.name + "/index.m3u8");
            if (auto r = upload(out / rung.name / "index.m3u8", key, Output::Playlist); !r) {
                return std::unexpected(r.error());
            }
            const std::uint32_t kbps = rung.video_kbps + (has_audio ? core::kAudioKbps : 0);
            renditions.push_back({.height = rung.height,
                                  .bitrate_bps = kbps * 1000,
                                  .playlist = *core::StorageKey::parse(key)});
        }
        if (auto r = upload(out / "master.m3u8", output_key(job_.video, "master.m3u8"),
                            Output::Playlist);
            !r) {
            return std::unexpected(r.error());
        }
        const auto wall = std::chrono::duration_cast<core::Millis>(deps_.clock.now() - started);
        const std::uint64_t bytes = published_bytes_.load();
        const double seconds =
            static_cast<double>(std::max<core::Millis::rep>(wall.count(), 1)) / 1000.0;
        log().info("published",
                   {{"job", id()},
                    {"files", published_.load()},
                    {"bytes", bytes},
                    {"wall_ms", wall.count()},
                    {"mib_per_s", static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds}});
        return renditions;
    }

    const JobDeps& deps_;
    const JobSettings& settings_;
    const core::ports::ClaimedJob& job_;
    Metrics metrics_;
    bool rerun_ = false;
    // Files of the output uploaded so far, of how many, and their bytes: for the progress
    // publishing reports and its log line. The counts are bumped from the publish pool.
    std::atomic<std::size_t> published_{0};
    std::size_t to_publish_ = 0;
    std::atomic<std::uint64_t> published_bytes_{0};
    std::stop_source abandon_;
    LeaseKeeper keeper_;
    std::stop_callback<RequestStop> on_shutdown_;
};

JobRunner::JobRunner(JobDeps deps, JobSettings settings)
    : deps_(std::move(deps)), settings_(std::move(settings)) {}

JobOutcome JobRunner::run(const core::ports::ClaimedJob& job, const std::stop_token& shutdown) {
    const auto started = deps_.clock.now();
    deps_.log.info("job claimed", {{"job", std::to_underlying(job.lease.job)},
                                   {"video", job.video.to_string()},
                                   {"fence", job.lease.fence},
                                   {"request_id", job.request_id}});
    JobOutcome outcome{};
    Metrics metrics;
    {
        Attempt attempt(deps_, settings_, job, shutdown);
        outcome = attempt.execute();
        metrics = attempt.metrics();
    }
    const auto wall = std::chrono::duration_cast<core::Millis>(deps_.clock.now() - started);
    const double realtime = metrics.transcode.count() > 0
                                ? static_cast<double>(metrics.media.count()) /
                                      static_cast<double>(metrics.transcode.count())
                                : 0.0;
    // One line per job with its numbers, carrying the id of the request that queued it.
    deps_.log.info("job finished", {{"job", std::to_underlying(job.lease.job)},
                                    {"request_id", job.request_id},
                                    {"outcome", to_string(outcome)},
                                    {"wall_ms", wall.count()},
                                    {"media_ms", metrics.media.count()},
                                    {"transcode_ms", metrics.transcode.count()},
                                    {"realtime", realtime},
                                    {"ffmpeg_exit", metrics.ffmpeg_exit},
                                    {"ffmpeg_peak_rss_kib", metrics.ffmpeg_peak_rss_kib},
                                    {"worker_peak_rss_kib", own_peak_rss_kib()}});
    return outcome;
}

} // namespace worker
