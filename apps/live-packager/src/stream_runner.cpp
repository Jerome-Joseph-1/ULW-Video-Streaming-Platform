#include "stream_runner.hpp"

#include "os/unique_fd.hpp"

#include "log.hpp"
#include "pipe.hpp"
#include "watch.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace live {

namespace {

namespace fs = std::filesystem;

// How often ffmpeg's playlist is looked at. A segment is up to a target duration old before
// it exists at all, so 25 ms of extra delay in seeing it is noise, and reading a playlist of
// a few hundred bytes 40 times a second is not a cost.
constexpr std::chrono::milliseconds kTick{25};
// ffmpeg's own playlist is a few hundred bytes a segment.
constexpr std::uintmax_t kMaxPlaylistBytes = std::uintmax_t{1} << 20U;

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

// Moves the publisher's payload into the pipe ffmpeg reads, and closes the pipe when the
// publisher is gone, which is ffmpeg's end of input.
void relay(infra::srt::Session& session, std::vector<std::byte> first, os::UniqueFd sink,
           const std::stop_token& stop) {
    if (!write_all(sink.get(), first, stop)) {
        return;
    }
    std::array<std::byte, infra::srt::kMaxPayload> buffer{};
    while (!stop.stop_requested()) {
        const auto read = session.read(buffer);
        if (read.status == infra::srt::ReadStatus::Closed) {
            return;
        }
        if (read.status == infra::srt::ReadStatus::Data &&
            !write_all(sink.get(), std::span(buffer).first(read.bytes), stop)) {
            return;
        }
    }
}

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

enum class FirstMediaStatus : std::uint8_t {
    Media,
    // A stop was requested first.
    Stopped,
    // The publisher connected and sent nothing for the whole patience.
    Silent,
    // The publisher disconnected without sending anything.
    Gone,
};

struct FirstMedia {
    FirstMediaStatus status = FirstMediaStatus::Stopped;
    std::vector<std::byte> bytes;
    core::WallTime at;
};

// Blocks until the publisher sends, which is when the media begins and what the playlist's
// wall-clock times are anchored to; a connection made ahead of the media must not skew them.
FirstMedia await_first_media(infra::srt::Session& session, const core::ports::IClock& clock,
                             std::chrono::seconds patience, const std::stop_token& stop) {
    const core::MonoTime start = clock.now();
    std::array<std::byte, infra::srt::kMaxPayload> buffer{};
    while (!stop.stop_requested()) {
        const auto read = session.read(buffer);
        if (read.status == infra::srt::ReadStatus::Data) {
            return {
                .status = FirstMediaStatus::Media,
                .bytes = {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read.bytes)},
                .at = clock.wall_now()};
        }
        if (read.status == infra::srt::ReadStatus::Closed) {
            return {.status = FirstMediaStatus::Gone, .bytes = {}, .at = {}};
        }
        if (clock.now() - start >= patience) {
            return {.status = FirstMediaStatus::Silent, .bytes = {}, .at = {}};
        }
    }
    return {.status = FirstMediaStatus::Stopped, .bytes = {}, .at = {}};
}

// The end of a run that never had media to publish: nothing ffmpeg wrote, so only an old
// window to end, if the process was asked to end the stream.
Outcome end_without_media(Publisher& publisher, const StopRequests& stops, bool wait_was_stopped,
                          bool failure) {
    // A drain leaves the stream as it is.
    if (wait_was_stopped && !stops.end.stop_requested()) {
        return Outcome::Ended;
    }
    const auto ended = publisher.finish({});
    return !ended.problem && !failure ? Outcome::Ended : Outcome::Failed;
}

// The end of a run that had media: reports how ffmpeg ended, then either leaves the stream
// open (a drain, which still uploads what ffmpeg finished) or ends its playlist.
Outcome conclude(Publisher& publisher, const StopRequests& stops, Verdict verdict,
                 const std::expected<infra::ffmpeg::LiveRemuxResult, std::string>& result,
                 std::string_view final_playlist) {
    bool failed = verdict != Verdict::Healthy;
    if (!result) {
        log("remuxer: {}", result.error());
        failed = true;
    } else {
        if (result->video_unprobed) {
            // ffmpeg's own last line says only that it could not open its output.
            log("ffmpeg found no video codec parameters in the first {} ms ({} bytes) of the "
                "stream, which must hold a keyframe: the publisher is to send one every "
                "segment length",
                result->probe.window.count(), result->probe.bytes);
        }
        log("{}, ffmpeg exited {} after {} ms, peak {} KiB{}{}", describe(result->end),
            result->signal != 0 ? 128 + result->signal : result->exit_code, result->wall.count(),
            result->peak_rss_kib, result->detail.empty() ? "" : ": ", result->detail);
        failed = failed || result->end == infra::ffmpeg::LiveEnd::Failed;
    }
    if (verdict == Verdict::Superseded) {
        log("a newer packager of this stream took over; this one writes nothing more");
        return Outcome::Failed;
    }
    // A drain leaves the stream as it is. Whatever ffmpeg finished is still worth uploading;
    // the next process for this stream continues from there.
    const bool drained = stops.drain.stop_requested() && !stops.end.stop_requested() && result &&
                         result->end == infra::ffmpeg::LiveEnd::Stopped && !failed;
    if (drained) {
        const auto flushed = publisher.pump(final_playlist);
        if (!flushed) {
            log("draining: {}", to_string(flushed.error()));
        }
        log("drained at segment {}; the stream is left to be continued",
            publisher.next_sequence() - 1);
        return flushed ? Outcome::Ended : Outcome::Failed;
    }
    const FinishResult finished = publisher.finish(final_playlist);
    if (finished.problem) {
        log("ending the stream: {}", to_string(*finished.problem));
        failed = true;
    }
    if (finished.problem == PublishError::Superseded) {
        log("a newer packager of this stream took over; this one writes nothing more");
    } else if (finished.ended) {
        log("stream ended at segment {}", publisher.next_sequence() - 1);
    }
    return failed ? Outcome::Failed : Outcome::Ended;
}

} // namespace

Outcome run_stream(Publisher& publisher, infra::srt::IngestListener& listener,
                   const infra::ffmpeg::LiveRemuxer& remuxer, const core::ports::IClock& clock,
                   const RunSettings& settings, const StopRequests& stops) {
    // Either request stops the wait for a publisher and ffmpeg.
    std::stop_source stop_all;
    const std::stop_callback on_drain(stops.drain,
                                      [&stop_all]() noexcept { stop_all.request_stop(); });
    const std::stop_callback on_end(stops.end, [&stop_all]() noexcept { stop_all.request_stop(); });

    auto accepted = listener.accept(stop_all.get_token());
    if (!accepted) {
        log("ingest: {}", accepted.error());
        return Outcome::Failed;
    }
    std::optional<infra::srt::Session> session = std::move(*accepted);
    if (!session) {
        log("stopped before a publisher connected");
        return end_without_media(publisher, stops, true, false);
    }
    const WatchLimits limits = watch_limits(settings.segment_seconds, settings.listed_segments);
    log("publisher connected, continuing at segment {} epoch {}", publisher.next_sequence(),
        publisher.epoch());

    FirstMedia first = await_first_media(*session, clock, limits.stall, stop_all.get_token());
    if (first.status != FirstMediaStatus::Media) {
        switch (first.status) {
        case FirstMediaStatus::Stopped:
            log("stopped before the publisher sent anything");
            break;
        case FirstMediaStatus::Silent:
            log("the publisher sent nothing for {} s", limits.stall.count());
            break;
        case FirstMediaStatus::Gone:
            log("the publisher disconnected before sending anything");
            break;
        case FirstMediaStatus::Media:
            break;
        }
        return end_without_media(publisher, stops, first.status == FirstMediaStatus::Stopped,
                                 first.status == FirstMediaStatus::Silent);
    }
    publisher.begin_epoch(first.at);

    auto pipe = make_pipe();
    if (!pipe) {
        log("pipe: {}", std::generic_category().message(errno));
        return Outcome::Failed;
    }
    std::stop_source remux_stop;
    const std::stop_callback pass_on(stop_all.get_token(),
                                     [&remux_stop]() noexcept { remux_stop.request_stop(); });
    // Built here: the thread starts while the loop below is already using the publisher.
    const infra::ffmpeg::LiveRemuxJob job{.input = pipe->read.get(),
                                          .out_dir = settings.media_dir,
                                          .segment_seconds = settings.segment_seconds,
                                          .listed_segments = settings.listed_segments,
                                          .first_sequence = publisher.next_sequence(),
                                          .epoch = publisher.epoch(),
                                          .max_kbps = settings.max_kbps,
                                          .max_duration = settings.max_duration};
    RemuxRun run;
    std::stop_source relay_stop;
    std::jthread relay_thread([&session, &first, &pipe, &relay_stop] {
        relay(*session, std::move(first.bytes), std::move(pipe->write), relay_stop.get_token());
    });
    std::jthread remux_thread([&] {
        auto result = remuxer.run(job, remux_stop.get_token());
        const std::lock_guard lock(run.mutex);
        run.result = std::move(result);
        run.done = true;
        run.cv.notify_all();
    });

    Verdict verdict = Verdict::Healthy;
    Watch watch(publisher, limits, clock.now());
    const fs::path playlist_file = settings.media_dir / std::string(infra::ffmpeg::kLivePlaylist);
    while (true) {
        {
            std::unique_lock lock(run.mutex);
            if (run.cv.wait_for(lock, kTick, [&run] { return run.done; })) {
                break;
            }
        }
        if (verdict == Verdict::Healthy) {
            verdict = watch.look(read_playlist(playlist_file), clock.now());
            if (verdict != Verdict::Healthy) {
                remux_stop.request_stop();
            }
        }
    }
    remux_thread.join();
    relay_stop.request_stop();
    relay_thread.join();
    pipe->read.reset();

    const auto last = read_playlist(playlist_file);
    return conclude(publisher, stops, verdict,
                    std::move(run.result).value_or(std::unexpected("remuxer did not report")),
                    last ? std::string_view(*last) : std::string_view{});
}

} // namespace live
