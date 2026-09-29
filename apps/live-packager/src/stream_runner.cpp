#include "stream_runner.hpp"

#include "log.hpp"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace live {

namespace {

namespace fs = std::filesystem;

// How often ffmpeg's playlist is looked at. A segment is up to a target duration old before
// it exists at all, so 25 ms of extra delay in seeing it is noise, and reading a playlist of
// a few hundred bytes 40 times a second is not a cost.
constexpr std::chrono::milliseconds kTick{25};
// A source that keyframes every target duration completes a segment each one; five target
// durations without a segment tolerate keyframes up to four apart, and a source further out
// than that produces segments no player will take.
constexpr std::uint32_t kStallSegments = 5;
// ffmpeg's own playlist is a few hundred bytes a segment.
constexpr std::uintmax_t kMaxPlaylistBytes = std::uintmax_t{1} << 20U;
// Progress is logged this often, so a 12 hour stream writes hundreds of lines, not tens of
// thousands.
constexpr std::uint64_t kLogEverySegments = 30;

std::optional<std::string> read_playlist(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size > kMaxPlaylistBytes) {
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    std::string text(size, '\0');
    in.read(text.data(), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        return std::nullopt;
    }
    return text;
}

// The remuxer runs on a thread of its own and reports here when it returns.
struct RemuxRun {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::optional<std::expected<infra::ffmpeg::LiveRemuxResult, std::string>> result;
};

class Progress {
public:
    void published(std::uint64_t count, const Publisher& publisher) {
        const std::uint64_t before = total_;
        total_ += count;
        if (count != 0 &&
            (before == 0 || total_ / kLogEverySegments != before / kLogEverySegments)) {
            log("segment {} published, {} this run", publisher.window().next_sequence() - 1,
                total_);
        }
    }

private:
    std::uint64_t total_ = 0;
};

// Follows publishing and decides when it has failed for good: the store keeps refusing
// uploads for a window's worth of segments, the playlists disagree with themselves, or no
// segment completes.
class Watch {
public:
    Watch(Publisher& publisher, const RunSettings& settings, core::MonoTime start)
        : publisher_(publisher),
          // Upload failures are given up on after one window of segments: ffmpeg lists twice
          // that (config.hpp), so the list has not yet moved past a segment still waiting to
          // go up.
          patience_(std::chrono::seconds(
              static_cast<std::int64_t>(settings.listed_segments / 2 * settings.segment_seconds))),
          stall_limit_(std::chrono::seconds(kStallSegments * settings.segment_seconds)),
          last_segment_(start) {}

    // One look at ffmpeg's playlist, nullopt while there is none. True while all is well.
    [[nodiscard]] bool look(const std::optional<std::string>& playlist, core::MonoTime now) {
        if (playlist && !pump(*playlist, now)) {
            return false;
        }
        if (now - last_segment_ >= stall_limit_) {
            log("no segment for {} s: the publisher's keyframes are too far apart or it stalled",
                stall_limit_.count());
            return false;
        }
        return true;
    }

private:
    bool pump(const std::string& playlist, core::MonoTime now) {
        const auto pumped = publisher_.pump(playlist);
        if (pumped) {
            failing_since_.reset();
            if (*pumped != 0) {
                last_segment_ = now;
                progress_.published(*pumped, publisher_);
            }
            return true;
        }
        if (!last_failure_log_ || now - *last_failure_log_ >= std::chrono::seconds(10)) {
            log("publish: {}", to_string(pumped.error()));
            last_failure_log_ = now;
        }
        if (pumped.error() != PublishError::UploadFailed) {
            return false;
        }
        if (!failing_since_) {
            failing_since_ = now;
        } else if (now - *failing_since_ >= patience_) {
            log("publish: giving up after {} s of failed uploads", patience_.count());
            return false;
        }
        return true;
    }

    Publisher& publisher_;
    std::chrono::seconds patience_;
    std::chrono::seconds stall_limit_;
    core::MonoTime last_segment_;
    Progress progress_;
    std::optional<core::MonoTime> failing_since_;
    std::optional<core::MonoTime> last_failure_log_;
};

const char* describe(infra::ffmpeg::LiveEnd end) {
    switch (end) {
    case infra::ffmpeg::LiveEnd::InputEnded:
        return "publisher disconnected";
    case infra::ffmpeg::LiveEnd::Stopped:
        return "stopped";
    case infra::ffmpeg::LiveEnd::TimedOut:
        return "reached the maximum duration";
    case infra::ffmpeg::LiveEnd::Failed:
        return "ffmpeg failed";
    }
    return "unknown";
}

} // namespace

Outcome run_stream(Publisher& publisher, IngestListener& listener,
                   const infra::ffmpeg::LiveRemuxer& remuxer, const core::ports::IClock& clock,
                   const RunSettings& settings, const std::stop_token& stop) {
    auto connection = listener.accept(stop);
    if (!connection) {
        log("ingest: {}", connection.error());
        return Outcome::Failed;
    }
    const fs::path playlist_file = settings.media_dir / std::string(infra::ffmpeg::kLivePlaylist);
    if (!*connection) {
        log("stopped before a publisher connected");
        return publisher.finish({}) ? Outcome::Ended : Outcome::Failed;
    }
    log("publisher connected, continuing at segment {} epoch {}", publisher.next_sequence(),
        publisher.epoch());
    publisher.begin_epoch();

    std::stop_source remux_stop;
    // Ours to end early on a failure of the store; the caller's stop is passed on.
    const std::stop_callback pass_on(stop, [&remux_stop]() noexcept { remux_stop.request_stop(); });
    // Built here: the thread starts while the loop below is already using the publisher.
    const infra::ffmpeg::LiveRemuxJob job{.input = connection->get(),
                                          .out_dir = settings.media_dir,
                                          .segment_seconds = settings.segment_seconds,
                                          .listed_segments = settings.listed_segments,
                                          .first_sequence = publisher.next_sequence(),
                                          .epoch = publisher.epoch(),
                                          .max_duration = settings.max_duration};
    RemuxRun run;
    std::jthread remux_thread([&] {
        auto result = remuxer.run(job, remux_stop.get_token());
        const std::lock_guard lock(run.mutex);
        run.result = std::move(result);
        run.done = true;
        run.cv.notify_all();
    });

    bool broken = false;
    Watch watch(publisher, settings, clock.now());
    while (true) {
        {
            std::unique_lock lock(run.mutex);
            if (run.cv.wait_for(lock, kTick, [&run] { return run.done; })) {
                break;
            }
        }
        if (!broken && !watch.look(read_playlist(playlist_file), clock.now())) {
            broken = true;
            remux_stop.request_stop();
        }
    }
    remux_thread.join();

    const auto result = std::move(run.result).value_or(std::unexpected("remuxer did not report"));
    bool failed = broken;
    if (!result) {
        log("remuxer: {}", result.error());
        failed = true;
    } else {
        log("{}{}, ffmpeg exited {} after {} ms, peak {} KiB{}{}", describe(result->end),
            result->end == infra::ffmpeg::LiveEnd::Failed ? "" : " (ends the stream)",
            result->signal != 0 ? 128 + result->signal : result->exit_code, result->wall.count(),
            result->peak_rss_kib, result->detail.empty() ? "" : ": ", result->detail);
        failed = failed || result->end == infra::ffmpeg::LiveEnd::Failed;
    }
    connection->reset();
    if (publisher.overlong_segments() != 0) {
        log("{} segments longer than the target duration: the publisher's keyframes are further "
            "apart than {} s",
            publisher.overlong_segments(), settings.segment_seconds);
    }
    const auto last = read_playlist(playlist_file);
    if (const auto ended = publisher.finish(last ? std::string_view(*last) : std::string_view{});
        !ended) {
        log("ending the stream: {}", to_string(ended.error()));
        return Outcome::Failed;
    }
    log("stream ended at segment {}", publisher.next_sequence() - 1);
    return failed ? Outcome::Failed : Outcome::Ended;
}

} // namespace live
