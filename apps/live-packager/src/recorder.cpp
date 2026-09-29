#include "recorder.hpp"

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "core/util/parse.hpp"
#include "infra/ffmpeg/live_remux.hpp"

#include "log.hpp"
#include "media_playlist.hpp"
#include "pipe.hpp"
#include "publisher.hpp"
#include "recording_plan.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace live {

namespace {

namespace fs = std::filesystem;
using infra::ffmpeg::AudioFormat;
using infra::ffmpeg::RecordingInput;
using infra::ffmpeg::RecordingRemuxJob;
using infra::ffmpeg::RemuxError;
using infra::ffmpeg::RemuxFailure;
using infra::postgres::RecordingRow;
using infra::postgres::RecordingStoreError;

// Our playlists are a few KB; a store answering with more is not our playlist.
constexpr std::uint64_t kMaxPlaylistBytes = std::uint64_t{1} << 20U;
// A read per 64 KiB of segment on its way into the pipe.
constexpr std::size_t kCopyBuffer = std::size_t{64} << 10U;
// A line per hour of stream at the shortest segment length (2 s).
constexpr std::uint64_t kProgressEvery = 1800;

enum class Severity : std::uint8_t { Transient, Permanent, Stopped, Superseded };

struct Problem {
    Severity severity = Severity::Transient;
    std::string detail;
};

template <class T> using Step = std::expected<T, Problem>;

std::unexpected<Problem> problem(Severity severity, std::string detail) {
    return std::unexpected(Problem{.severity = severity, .detail = std::move(detail)});
}

Problem from_remux(const RemuxError& error, std::string_view what) {
    switch (error.kind) {
    case RemuxFailure::Unavailable:
        return {.severity = Severity::Transient, .detail = std::string(what) + ": " + error.detail};
    case RemuxFailure::Refused:
        return {.severity = Severity::Permanent, .detail = std::string(what) + ": " + error.detail};
    case RemuxFailure::Stopped:
        return {.severity = Severity::Stopped, .detail = "stopped"};
    }
    return {.severity = Severity::Transient, .detail = std::string(what)};
}

const core::ContentType& recording_type() {
    static const auto type = *core::ContentType::parse("video/mp2t");
    return type;
}

Step<core::StorageKey> key_in(const StreamId& stream, std::string_view name) {
    auto key = core::StorageKey::parse(stream.key_prefix() + std::string(name));
    if (!key) {
        return problem(Severity::Permanent, "unaddressable name " + std::string(name));
    }
    return std::move(*key);
}

// The stream's stored playlist; nullopt when it has none.
Step<std::optional<MediaPlaylist>> read_playlist(core::ports::IObjectTransfer& store,
                                                 const StreamId& stream, const fs::path& dir) {
    const auto key = key_in(stream, "index.m3u8");
    if (!key) {
        return std::unexpected(key.error());
    }
    const fs::path file = dir / "index.m3u8";
    const auto size = store.download(*key, file);
    if (!size) {
        if (size.error() == core::ports::StorageError::NotFound) {
            return std::nullopt;
        }
        return problem(Severity::Transient, "playlist unreadable");
    }
    if (*size > kMaxPlaylistBytes) {
        return problem(Severity::Permanent, "stored playlist too large");
    }
    std::string text(*size, '\0');
    std::ifstream in(file, std::ios::binary);
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    auto playlist = parse_media_playlist(text);
    if (!in || !playlist) {
        return problem(Severity::Permanent, "stored playlist invalid");
    }
    return std::move(*playlist);
}

class Recording {
public:
    Recording(const RecorderDeps& deps, const RecorderSettings& settings, std::stop_token stop)
        : deps_(deps), settings_(settings), stop_(std::move(stop)),
          pieces_(settings.work_dir / "pieces"), child_dir_(settings.work_dir / "child") {}

    [[nodiscard]] Step<void> prepare() {
        std::error_code ec;
        fs::remove_all(settings_.work_dir, ec);
        fs::create_directories(pieces_, ec);
        fs::create_directories(child_dir_, ec);
        if (ec) {
            return problem(Severity::Transient, "work dir: " + ec.message());
        }
        return {};
    }

    // No run newer than the one that ended the stream has claimed it. The ender is the newest
    // of the last segment's run, the run named in `ended_by`, and this process: the latter two
    // are above the first when a run ended the stream without a segment of its own (it claimed
    // and found the stream ended, or was ended before a publisher came). `ended_by` keeps that
    // across a restart, which has no claim of its own. A claim above the ender belongs to a
    // packager still publishing, which will overwrite the stale end this one saw.
    [[nodiscard]] Step<void> fence(const MediaPlaylist& ended) {
        const auto last = infra::ffmpeg::live_init_epoch(ended.segments.back().init);
        if (!last) {
            return problem(Severity::Permanent, "stored playlist invalid");
        }
        const auto ender = ended_by();
        if (!ender) {
            return std::unexpected(ender.error());
        }
        const std::uint32_t newest =
            std::max({*last, ender->value_or(*last), settings_.own_claim.value_or(*last)});
        const std::uint32_t next = newest + 1;
        const auto claim = key_in(settings_.stream, "epoch_" + std::to_string(next));
        if (!claim) {
            return std::unexpected(claim.error());
        }
        const auto seen = deps_.store.size(*claim);
        if (seen) {
            return problem(Severity::Superseded, "epoch " + std::to_string(next) + " is claimed");
        }
        if (seen.error() != core::ports::StorageError::NotFound) {
            return problem(Severity::Transient, "claim unreadable");
        }
        return {};
    }

    // The epoch the stream's ender wrote down; nullopt when none did (a stream ended before
    // this was written down, which the playlist's own last run then stands for).
    [[nodiscard]] Step<std::optional<std::uint32_t>> ended_by() {
        const auto key = key_in(settings_.stream, kEndedByName);
        if (!key) {
            return std::unexpected(key.error());
        }
        const fs::path file = pieces_ / kEndedByName;
        const auto got = deps_.store.download(*key, file);
        if (!got) {
            if (got.error() == core::ports::StorageError::NotFound) {
                return std::nullopt;
            }
            return problem(Severity::Transient, "ended_by unreadable");
        }
        // A decimal epoch and a newline. Anything else is as good as none: the playlist's last
        // run and this process's claim still bound the ender from below.
        constexpr std::uint64_t kMaxBytes = 16;
        if (*got > kMaxBytes) {
            log("recording: ended_by unreadable, ignored");
            return std::nullopt;
        }
        std::string text(*got, '\0');
        std::ifstream in(file, std::ios::binary);
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        while (text.ends_with('\n')) {
            text.pop_back();
        }
        const auto epoch = core::parse_integer<std::uint32_t>(text);
        if (!in || !epoch) {
            log("recording: ended_by unreadable, ignored");
            return std::nullopt;
        }
        return *epoch;
    }

    // The playlist still ends where it did, and the fence still holds: the recording covers
    // the stream to its real end.
    [[nodiscard]] Step<void> confirm(const MediaPlaylist& ended) {
        const auto now = read_playlist(deps_.store, settings_.stream, pieces_);
        if (!now) {
            return std::unexpected(now.error());
        }
        const auto end_of = [](const MediaPlaylist& p) {
            return p.media_sequence + p.segments.size();
        };
        const std::optional<MediaPlaylist>& stored = *now;
        if (!stored || !stored->ended || stored->segments.empty() ||
            end_of(*stored) != end_of(ended) ||
            stored->segments.back().uri != ended.segments.back().uri) {
            return problem(Severity::Superseded, "the playlist changed under the recording");
        }
        return fence(ended);
    }

    [[nodiscard]] Step<RecordResult> record(const MediaPlaylist& ended) {
        if (auto fenced = fence(ended); !fenced) {
            return std::unexpected(fenced.error());
        }
        const auto plan = plan_recording(ended, settings_.stream, deps_.store);
        if (!plan) {
            return plan.error() == PlanError::StoreUnreadable
                       ? problem(Severity::Transient, "store unreadable while planning")
                       : problem(Severity::Permanent, "stored playlist invalid");
        }
        if (auto probed = probe(*plan); !probed) {
            return std::unexpected(probed.error());
        }
        const auto video = core::VideoId::generate(deps_.clock, deps_.random);
        // Where an upload's source goes (the gateway's), so the job is the same as for one.
        const auto key = core::StorageKey::parse("videos/" + video.to_string() + "/raw");
        if (!key) {
            return problem(Severity::Permanent, "unaddressable source key");
        }
        if (auto made = assemble(*plan, *key); !made) {
            return std::unexpected(made.error());
        }
        if (auto confirmed = confirm(ended); !confirmed) {
            discard(*key);
            return std::unexpected(confirmed.error());
        }
        const auto row = deps_.catalog.record({.stream = settings_.stream.str(),
                                               .video = video,
                                               .owner = settings_.owner,
                                               .title = "Live stream " + settings_.stream.str(),
                                               .source = *key});
        if (!row) {
            return unrecorded(video, *key, row.error() == RecordingStoreError::Unknown);
        }
        if (row->video == video) {
            return RecordResult{.outcome = RecordOutcome::Recorded, .video = video, .detail = {}};
        }
        // Another recorder's row went in first; ours is an object nothing will read.
        discard(*key);
        return RecordResult{
            .outcome = RecordOutcome::AlreadyRecorded, .video = row->video, .detail = row->failure};
    }

private:
    struct Stopper {
        std::stop_source* source;
        void operator()() const noexcept { source->request_stop(); }
    };

    // The insert failed. When it failed before its commit nothing was written; when the commit's
    // answer was lost (`maybe_written`), it may have landed, or may land yet on the session that
    // sent it. The object is removed only when the stream's row names another outcome, or, for
    // an insert known not to have been written, when there is no row.
    [[nodiscard]] Step<RecordResult> unrecorded(const core::VideoId& video,
                                                const core::StorageKey& key, bool maybe_written) {
        const auto known = deps_.catalog.find(settings_.stream.str());
        if (!known) {
            return problem(Severity::Transient, "database unavailable; recording kept");
        }
        const std::optional<RecordingRow>& row = *known;
        if (row && row->video == video) {
            return RecordResult{.outcome = RecordOutcome::Recorded, .video = video, .detail = {}};
        }
        if (row) {
            discard(key);
            return RecordResult{.outcome = RecordOutcome::AlreadyRecorded,
                                .video = row->video,
                                .detail = row->failure};
        }
        if (maybe_written) {
            return problem(Severity::Transient, "insert outcome unknown; recording kept");
        }
        discard(key);
        return problem(Severity::Transient, "database unavailable");
    }

    void discard(const core::StorageKey& key) {
        if (!deps_.streams.remove(key)) {
            log("recording: could not remove {}", key.str());
        }
    }

    [[nodiscard]] fs::path init_file(std::uint32_t epoch) const {
        return pieces_ / infra::ffmpeg::live_init_name(epoch);
    }

    // Downloads `name` of the stream to `file`; a missing object is for good, since the
    // packager never deletes one and the bucket's rule only expires them.
    [[nodiscard]] Step<void> fetch(std::string_view name, const fs::path& file) {
        const auto key = key_in(settings_.stream, name);
        if (!key) {
            return std::unexpected(key.error());
        }
        const auto got = deps_.store.download(*key, file);
        if (got) {
            return {};
        }
        if (got.error() == core::ports::StorageError::NotFound) {
            return problem(Severity::Permanent, std::string(name) + " is missing");
        }
        return problem(Severity::Transient, std::string(name) + " unreadable");
    }

    // Each run's audio, and the stream's: the first a run carries. A run without it gets
    // silence of that format, so the joined recording keeps one audio stream throughout.
    [[nodiscard]] Step<void> probe(const RecordingPlan& plan) {
        for (const RecordingRun& run : plan.runs) {
            if (formats_.contains(run.epoch)) {
                continue;
            }
            if (auto got = fetch(infra::ffmpeg::live_init_name(run.epoch), init_file(run.epoch));
                !got) {
                return got;
            }
            const auto format = deps_.copier.probe_audio(init_file(run.epoch), child_dir_, stop_);
            if (!format) {
                return std::unexpected(from_remux(format.error(), "probing the audio"));
            }
            formats_[run.epoch] = *format;
            if (*format && !stream_audio_) {
                stream_audio_ = *format;
            }
        }
        return {};
    }

    [[nodiscard]] RecordingRemuxJob job(RecordingInput from, int input,
                                        std::optional<AudioFormat> silence) const {
        return {.from = from,
                .input = input,
                .work_dir = child_dir_,
                .wall = settings_.wall,
                .cpu = infra::ffmpeg::recording_copy_cpu(settings_.max_bytes, settings_.wall),
                .silence = silence};
    }

    // Two stages of ffmpeg. Each run's init segment and segments are one fragmented MP4 and go
    // through a first ffmpeg of their own, which makes MPEG-TS of them; every run's TS goes
    // through the second, one for the whole recording, which carries the timeline across the
    // jump where a restarted run's timestamps begin again, and whose output streams into the
    // store. Concatenated fMP4 of two runs would not do: ffmpeg skips the second moov and reads
    // the second run's fragments on the first run's timeline, backwards.
    [[nodiscard]] Step<void> assemble(const RecordingPlan& plan, const core::StorageKey& key) {
        auto output = deps_.streams.begin(key, recording_type(), settings_.max_bytes);
        if (!output) {
            return problem(Severity::Transient, "the store refused the recording");
        }
        output_ = output->get();
        const std::stop_callback on_stop(stop_, Stopper{&abort_});
        auto joined = make_pipe();
        if (!joined) {
            return problem(Severity::Transient, "pipe");
        }
        std::expected<void, RemuxError> joining;
        std::jthread joiner([&] {
            joining = deps_.copier.run(
                job(RecordingInput::MpegTs, joined->read.get(), {}),
                [this](std::string_view out) noexcept { keep(out); }, abort_.get_token());
            if (!joining) {
                abort_.request_stop();
            }
        });
        Step<void> fed;
        for (const RecordingRun& run : plan.runs) {
            fed = feed(run, joined->write.get());
            if (!fed) {
                break;
            }
        }
        // A run that could not be fed must not look to the joining stage like the end of its
        // input: it would judge, and perhaps refuse, a truncated recording.
        if (!fed) {
            abort_.request_stop();
        }
        joined->write.reset();
        joiner.join();
        if (stop_.stop_requested()) {
            return problem(Severity::Stopped, "stopped");
        }
        if (past_bound_) {
            return problem(Severity::Permanent, "the recording is past its bound of " +
                                                    std::to_string(settings_.max_bytes) + " bytes");
        }
        // Whatever the store answers, a credential, a bucket or a disk can be put right, so no
        // store error condemns the stream.
        if (upload_error_) {
            return problem(Severity::Transient,
                           "upload: " + std::string(core::ports::to_string(*upload_error_)));
        }
        // A stage that failed stops the other, which then reports being stopped: the one that
        // did not is the cause.
        if (!joining && joining.error().kind != RemuxFailure::Stopped) {
            return std::unexpected(from_remux(joining.error(), "joining the runs"));
        }
        if (!fed) {
            return fed;
        }
        if (!joining) {
            return std::unexpected(from_remux(joining.error(), "joining the runs"));
        }
        if (auto committed = (*output)->commit(); !committed) {
            return problem(Severity::Transient,
                           "upload: " + std::string(core::ports::to_string(committed.error())));
        }
        log("recording: {} segments in {} runs, {} missing, {} bytes", poured_, plan.runs.size(),
            plan.missing, bytes_);
        return {};
    }

    // On the joining stage's thread only, until it is joined. The store's write may throw
    // (allocating a part); nothing may leave the stage's thread.
    void keep(std::string_view out) noexcept {
        try {
            keep_or_throw(out);
        } catch (...) {
            upload_error_ = core::ports::StorageError::Transient;
            abort_.request_stop();
        }
    }

    void keep_or_throw(std::string_view out) {
        if (upload_error_ || past_bound_) {
            return;
        }
        if (out.size() > settings_.max_bytes - bytes_) {
            past_bound_ = true;
            abort_.request_stop();
            return;
        }
        if (auto written = output_->write(std::as_bytes(std::span(out))); !written) {
            upload_error_ = written.error();
            abort_.request_stop();
            return;
        }
        bytes_ += out.size();
    }

    [[nodiscard]] Step<void> feed(const RecordingRun& run, int joined) {
        auto pipe = make_pipe();
        if (!pipe) {
            return problem(Severity::Transient, "pipe");
        }
        const auto& own = formats_[run.epoch];
        const std::optional<AudioFormat> silence =
            !own && stream_audio_ ? stream_audio_ : std::nullopt;
        std::expected<void, RemuxError> copied;
        std::jthread copier([&] {
            copied = deps_.copier.run(
                job(RecordingInput::FragmentedMp4, pipe->read.get(), silence),
                [this, joined](std::string_view out) noexcept {
                    if (!write_all(joined, std::as_bytes(std::span(out)), abort_.get_token())) {
                        abort_.request_stop();
                    }
                },
                abort_.get_token());
            if (!copied) {
                abort_.request_stop();
            }
        });
        auto poured = pour_run(run, pipe->write.get());
        pipe->write.reset();
        copier.join();
        if (!poured) {
            return poured;
        }
        if (!copied) {
            return std::unexpected(
                from_remux(copied.error(), "copying epoch " + std::to_string(run.epoch)));
        }
        return {};
    }

    [[nodiscard]] Step<void> pour_run(const RecordingRun& run, int sink) {
        if (auto poured = pour(init_file(run.epoch), sink); !poured) {
            return poured;
        }
        const fs::path piece = pieces_ / "segment";
        for (std::uint64_t n = run.first; n <= run.last && !abort_.stop_requested(); ++n) {
            if (auto got = fetch(infra::ffmpeg::live_segment_name(run.epoch, n), piece); !got) {
                return got;
            }
            if (auto poured = pour(piece, sink); !poured) {
                return poured;
            }
            if (++poured_ % kProgressEvery == 0) {
                log("recording: {} segments copied", poured_);
            }
        }
        return {};
    }

    // Pours a downloaded piece into `sink`. A pipe that will take no more means the ffmpeg
    // reading it has stopped, which the caller learns from its result.
    [[nodiscard]] Step<void> pour(const fs::path& file, int sink) {
        const os::UniqueFd in(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
        if (!in) {
            return problem(Severity::Transient, "piece unreadable");
        }
        std::array<std::byte, kCopyBuffer> buffer{};
        while (true) {
            const ssize_t n = ::read(in.get(), buffer.data(), buffer.size());
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0) {
                return problem(Severity::Transient, "piece unreadable");
            }
            if (n == 0) {
                return {};
            }
            if (!write_all(sink, std::span(buffer).first(static_cast<std::size_t>(n)),
                           abort_.get_token())) {
                return {};
            }
        }
    }

    const RecorderDeps& deps_;
    const RecorderSettings& settings_;
    std::stop_token stop_;
    // Where the parent keeps what it downloads, out of the children's reach.
    fs::path pieces_;
    // The children's writable directory, empty.
    fs::path child_dir_;
    std::map<std::uint32_t, std::optional<AudioFormat>> formats_;
    std::optional<AudioFormat> stream_audio_;
    // Fires on the caller's stop and on any failure, and stops every stage.
    std::stop_source abort_;
    core::ports::IObjectStream* output_ = nullptr;
    std::optional<core::ports::StorageError> upload_error_;
    bool past_bound_ = false;
    std::uint64_t bytes_ = 0;
    std::uint64_t poured_ = 0;
};

RecordResult result(RecordOutcome outcome, std::string detail,
                    std::optional<core::VideoId> video = std::nullopt) {
    return {.outcome = outcome, .video = video, .detail = std::move(detail)};
}

RecordResult from_row(const RecordingRow& row) {
    return result(RecordOutcome::AlreadyRecorded, row.failure, row.video);
}

// What a problem ends the run with, unless it is one no retry fixes.
std::optional<RecordResult> passing(const Problem& p) {
    switch (p.severity) {
    case Severity::Superseded:
        return result(RecordOutcome::Superseded, p.detail);
    case Severity::Transient:
    case Severity::Stopped:
        return result(RecordOutcome::Failed, p.detail);
    case Severity::Permanent:
        return std::nullopt;
    }
    return result(RecordOutcome::Failed, p.detail);
}

// A problem that no retry fixes is written down, so no run tries again; unless the end it was
// found on turns out to be a stale writer's, whose stream goes on.
RecordResult settle(const RecorderDeps& deps, const RecorderSettings& settings,
                    Recording& recording, const MediaPlaylist& ended, const Problem& p) {
    if (auto done = passing(p)) {
        return std::move(*done);
    }
    if (auto confirmed = recording.confirm(ended); !confirmed) {
        if (auto done = passing(confirmed.error())) {
            return std::move(*done);
        }
    }
    const auto row = deps.catalog.fail(settings.stream.str(), p.detail);
    if (!row) {
        return result(RecordOutcome::Failed, "database unavailable; " + p.detail);
    }
    if (row->video) {
        return from_row(*row);
    }
    return result(RecordOutcome::Unrecordable, row->failure);
}

} // namespace

std::string_view to_string(RecordOutcome outcome) noexcept {
    switch (outcome) {
    case RecordOutcome::Recorded:
        return "recorded";
    case RecordOutcome::AlreadyRecorded:
        return "already recorded";
    case RecordOutcome::NothingToRecord:
        return "nothing to record";
    case RecordOutcome::Superseded:
        return "superseded";
    case RecordOutcome::Unrecordable:
        return "unrecordable";
    case RecordOutcome::Failed:
        return "failed";
    }
    return "failed";
}

std::uint64_t recording_bound(std::uint32_t max_kbps, core::Seconds max_duration) noexcept {
    constexpr std::uint64_t kBytesPerKbit = 125;
    const std::uint64_t media =
        std::uint64_t{max_kbps} * kBytesPerKbit * static_cast<std::uint64_t>(max_duration.count());
    return media + (media / 8);
}

RecordResult record_stream(const RecorderDeps& deps, const RecorderSettings& settings,
                           const std::stop_token& stop) {
    const auto known = deps.catalog.find(settings.stream.str());
    if (!known) {
        return result(RecordOutcome::Failed, "database unavailable");
    }
    if (const std::optional<RecordingRow>& row = *known; row) {
        return from_row(*row);
    }
    Recording recording(deps, settings, stop);
    if (auto ready = recording.prepare(); !ready) {
        return result(RecordOutcome::Failed, ready.error().detail);
    }
    const auto stored = read_playlist(deps.store, settings.stream, settings.work_dir);
    if (!stored) {
        // Nothing is known of the stream's end yet, so nothing is marked either.
        return result(stored.error().severity == Severity::Permanent ? RecordOutcome::Unrecordable
                                                                     : RecordOutcome::Failed,
                      stored.error().detail);
    }
    const std::optional<MediaPlaylist>& playlist = *stored;
    if (!playlist || !playlist->ended || playlist->segments.empty()) {
        return result(RecordOutcome::NothingToRecord, {});
    }
    const MediaPlaylist& ended = *playlist;
    auto done = recording.record(ended);
    if (!done) {
        return settle(deps, settings, recording, ended, done.error());
    }
    return std::move(*done);
}

} // namespace live
