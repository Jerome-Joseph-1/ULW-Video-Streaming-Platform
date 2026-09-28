#include "job_runner.hpp"

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"

#include "log.hpp"
#include "workspace.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
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

class KeeperProgress final : public core::ports::ITranscodeProgress {
public:
    KeeperProgress(LeaseKeeper& keeper, core::Millis duration) noexcept
        : keeper_(keeper), duration_(duration) {}

    void on_progress(core::Millis encoded) noexcept override {
        const auto percent = std::clamp<core::Millis::rep>(
            encoded.count() * 100 / std::max<core::Millis::rep>(duration_.count(), 1), 0, 100);
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
        return reran ? Disposition::FailPermanently : Disposition::RerunOnce;
    case TranscodeFailure::Killed:
    case TranscodeFailure::Sandbox:
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
          keeper_(deps.lease_queue, settings.node, job.lease, settings.lease, abandon_),
          on_shutdown_(shutdown, RequestStop{&abandon_}) {}

    JobOutcome execute() {
        const auto size = deps_.store.size(job_.source);
        if (!size) {
            return storage_failure("source size", size.error());
        }
        auto workspace = Workspace::create(settings_.scratch, *size, deps_.random);
        if (!workspace) {
            const bool full = workspace.error() == WorkspaceError::InsufficientSpace;
            log("job={} workspace: {}", id(), full ? "not enough free space" : "scratch unusable");
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
        log("job={} probed {}x{} {}/{} fps {} ms audio={} rungs={}", id(), media->width,
            media->height, media->frame_rate.num, media->frame_rate.den, media->duration.count(),
            media->has_audio, ladder.size());
        // The source is on disk by now, so what is free has to hold the output alone.
        const std::uint64_t needed = output_bytes(media->duration, ladder, media->has_audio);
        if (const auto available = deps_.free_space(workspace->dir());
            !available || *available < needed) {
            log("job={} workspace: {} bytes free, the output needs {}", id(), available.value_or(0),
                needed);
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

        const auto verified =
            deps_.transcoder.verify(workspace->output(), *media, ladder, abandon_.get_token());
        if (!verified) {
            return transcode_failure("verify", verified.error(), *workspace);
        }
        if (abandoned()) {
            return stop_outcome();
        }
        // The last cheap chance to find out we are a zombie before writing any object.
        if (const auto held = deps_.queue.heartbeat(job_.lease, settings_.node); held && !*held) {
            log("job={} lease lost before publishing: heartbeat matched no row", id());
            return JobOutcome::FencedOut;
        }
        auto renditions = publish(workspace->output(), ladder, media->has_audio);
        if (!renditions && keeper_.lost()) {
            log("job={} publish stopped: {}", id(), renditions.error());
            return JobOutcome::Abandoned;
        }
        if (!renditions) {
            log("job={} publish: {}", id(), renditions.error());
            return fail("publishing the output failed", /*retryable=*/true);
        }
        const auto finished = deps_.queue.finish(job_.lease, media->duration, *renditions);
        if (!finished) {
            log("job={} finish: the job queue call failed", id());
            return JobOutcome::Unrecorded;
        }
        if (!*finished) {
            log("job={} finish matched no row: fenced out", id());
            return JobOutcome::FencedOut;
        }
        return JobOutcome::Done;
    }

    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }

private:
    [[nodiscard]] std::int64_t id() const noexcept { return std::to_underlying(job_.lease.job); }
    [[nodiscard]] bool abandoned() const noexcept { return abandon_.stop_requested(); }

    // Runs `step` again once when its first failure calls for it.
    template <class Step> std::invoke_result_t<Step&> with_rerun(Step step) {
        auto result = step();
        if (!result && disposition(result.error().kind, false) == Disposition::RerunOnce) {
            log("job={} {}; running it once more", id(), result.error().detail);
            result = step();
            rerun_ = true;
        }
        return result;
    }

    JobOutcome transcode_failure(std::string_view step, const TranscodeError& error,
                                 const Workspace& workspace) {
        log("job={} {} failed: {}", id(), step, error.detail);
        if (error.kind != TranscodeFailure::Stopped) {
            if (const auto left = deps_.free_space(workspace.dir()); left && *left < kNearlyFull) {
                log("job={} workspace: {} bytes left, the disk filled up", id(), *left);
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
        log("job={} {}: {}", id(), step, core::ports::to_string(error));
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
            log("job={} fail: the job queue call failed", id());
            return JobOutcome::Unrecorded;
        }
        if (!*written) {
            log("job={} fail matched no row: fenced out", id());
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
        if (!fs::is_regular_file(fs::symlink_status(file, ec))) {
            return std::unexpected("not a regular file: " + file.filename().string());
        }
        const auto parsed = core::StorageKey::parse(key);
        if (!parsed) {
            return std::unexpected("unaddressable output " + key);
        }
        if (auto r = deps_.store.upload(file, *parsed, content_type(kind)); !r) {
            return std::unexpected(key + ": " + std::string(core::ports::to_string(r.error())));
        }
        return {};
    }

    // A rung's init and media segments, in name order.
    std::expected<void, std::string> publish_segments(const fs::path& dir,
                                                      const std::string& rung) {
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
            if (auto r =
                    upload(file, output_key(job_.video, std::format("{}/{}", rung, name)), *kind);
                !r) {
                return r;
            }
        }
        return {};
    }

    // Segments, then media playlists, then the master: a reader who finds a playlist finds
    // everything it names, and the master, written last, is the commit point.
    std::expected<std::vector<core::ports::Rendition>, std::string>
    publish(const fs::path& out, std::span<const core::Rung> ladder, bool has_audio) {
        for (const core::Rung& rung : ladder) {
            if (auto r = publish_segments(out / rung.name, rung.name); !r) {
                return std::unexpected(r.error());
            }
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
        return renditions;
    }

    const JobDeps& deps_;
    const JobSettings& settings_;
    const core::ports::ClaimedJob& job_;
    Metrics metrics_;
    bool rerun_ = false;
    std::stop_source abandon_;
    LeaseKeeper keeper_;
    std::stop_callback<RequestStop> on_shutdown_;
};

JobRunner::JobRunner(JobDeps deps, JobSettings settings)
    : deps_(std::move(deps)), settings_(std::move(settings)) {}

JobOutcome JobRunner::run(const core::ports::ClaimedJob& job, const std::stop_token& shutdown) {
    const auto started = deps_.clock.now();
    log("job={} claimed video={} fence={}", std::to_underlying(job.lease.job),
        job.video.to_string(), job.lease.fence);
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
    log("job={} outcome={} wall_ms={} media_ms={} transcode_ms={} realtime={:.2f}x ffmpeg_exit={} "
        "ffmpeg_peak_rss_kib={} worker_peak_rss_kib={}",
        std::to_underlying(job.lease.job), to_string(outcome), wall.count(), metrics.media.count(),
        metrics.transcode.count(), realtime, metrics.ffmpeg_exit, metrics.ffmpeg_peak_rss_kib,
        own_peak_rss_kib());
    return outcome;
}

} // namespace worker
