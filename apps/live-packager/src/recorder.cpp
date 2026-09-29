#include "recorder.hpp"

#include "core/models/content_type.hpp"
#include "core/models/storage_key.hpp"
#include "infra/ffmpeg/live_remux.hpp"

#include "log.hpp"
#include "media_playlist.hpp"
#include "pipe.hpp"
#include "recording_plan.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace live {

namespace {

namespace fs = std::filesystem;
using infra::ffmpeg::RecordingInput;
using infra::ffmpeg::RecordingRemuxJob;

// Our playlists are a few KB; a store answering with more is not our playlist.
constexpr std::uint64_t kMaxPlaylistBytes = std::uint64_t{1} << 20U;
// A read per 64 KiB of segment on its way into the pipe.
constexpr std::size_t kCopyBuffer = std::size_t{64} << 10U;

const core::ContentType& recording_type() {
    static const auto type = *core::ContentType::parse("video/mp2t");
    return type;
}

std::expected<core::StorageKey, RecordError> key_in(const StreamId& stream, std::string_view name) {
    auto key = core::StorageKey::parse(stream.key_prefix() + std::string(name));
    if (!key) {
        return std::unexpected(RecordError::PlaylistInvalid);
    }
    return std::move(*key);
}

// The stored playlist, if it ends with segments; nullopt when there is nothing to record.
std::expected<std::optional<MediaPlaylist>, RecordError>
read_ended(core::ports::IObjectTransfer& store, const StreamId& stream, const fs::path& dir) {
    const auto key = key_in(stream, "index.m3u8");
    if (!key) {
        return std::unexpected(key.error());
    }
    const fs::path file = dir / "ended.m3u8";
    const auto size = store.download(*key, file);
    if (!size) {
        if (size.error() == core::ports::StorageError::NotFound) {
            return std::nullopt;
        }
        return std::unexpected(RecordError::StoreUnreadable);
    }
    if (*size > kMaxPlaylistBytes) {
        return std::unexpected(RecordError::PlaylistInvalid);
    }
    std::string text(*size, '\0');
    std::ifstream in(file, std::ios::binary);
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    auto playlist = parse_media_playlist(text);
    if (!in || !playlist) {
        return std::unexpected(RecordError::PlaylistInvalid);
    }
    if (!playlist->ended || playlist->segments.empty()) {
        return std::nullopt;
    }
    return std::move(*playlist);
}

// Downloads one stored object and pours it into `sink`.
std::expected<void, RecordError> pour(core::ports::IObjectTransfer& store,
                                      const core::StorageKey& key, const fs::path& file, int sink,
                                      const std::stop_token& stop) {
    if (!store.download(key, file)) {
        return std::unexpected(RecordError::StoreUnreadable);
    }
    const os::UniqueFd in(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
    if (!in) {
        return std::unexpected(RecordError::StoreUnreadable);
    }
    std::array<std::byte, kCopyBuffer> buffer{};
    while (true) {
        const ssize_t n = ::read(in.get(), buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return std::unexpected(RecordError::StoreUnreadable);
        }
        if (n == 0) {
            return {};
        }
        if (!write_all(sink, std::span(buffer).first(static_cast<std::size_t>(n)), stop)) {
            return std::unexpected(stop.stop_requested() ? RecordError::Stopped
                                                         : RecordError::RemuxFailed);
        }
    }
}

// Two stages of ffmpeg, both copies. Each run's init segment and segments are one fragmented
// MP4 and go through a first ffmpeg of their own, which makes MPEG-TS of them; every run's TS
// goes through the second, one for the whole recording, which carries the timeline across the
// jump where a restarted run's timestamps begin again, and whose output streams into the store.
// Concatenated fMP4 of two runs would not do: ffmpeg skips the second moov and reads the second
// run's fragments on the first run's timeline, backwards.
class Assembly {
public:
    Assembly(const RecorderDeps& deps, const RecorderSettings& settings,
             core::ports::IObjectStream& output, const std::stop_token& stop)
        : deps_(deps), settings_(settings), output_(output),
          on_stop_(stop, Abort{.abort = &abort_, .stopped = &caller_stopped_}) {}

    std::expected<std::uint64_t, RecordError> run(const RecordingPlan& plan) {
        auto joined = make_pipe();
        if (!joined) {
            return std::unexpected(RecordError::RemuxFailed);
        }
        std::expected<void, std::string> joining;
        std::jthread joiner([&] {
            joining = deps_.remuxer.run(
                job(RecordingInput::MpegTs, joined->read.get()),
                [this](std::string_view out) { keep(out); }, abort_.get_token());
            if (!joining) {
                abort_.request_stop();
            }
        });
        std::expected<void, RecordError> fed;
        for (const RecordingRun& run : plan.runs) {
            fed = feed(run, joined->write.get());
            if (!fed) {
                break;
            }
        }
        joined->write.reset();
        joiner.join();
        if (!fed) {
            return std::unexpected(fed.error());
        }
        if (!joining) {
            log("recording: joining the runs failed: {}", joining.error());
        }
        return finish(joining.has_value());
    }

private:
    struct Abort {
        std::stop_source* abort;
        std::stop_source* stopped;
        void operator()() const noexcept {
            stopped->request_stop();
            abort->request_stop();
        }
    };

    [[nodiscard]] RecordingRemuxJob job(RecordingInput from, int input) const {
        return {.from = from,
                .input = input,
                .work_dir = settings_.work_dir,
                .budget = settings_.budget};
    }

    // On the joining stage's thread only, until it is joined.
    void keep(std::string_view out) {
        if (upload_error_) {
            return;
        }
        if (auto written = output_.write(std::as_bytes(std::span(out))); !written) {
            upload_error_ = written.error();
            abort_.request_stop();
            return;
        }
        bytes_ += out.size();
    }

    std::expected<void, RecordError> feed(const RecordingRun& run, int joined) {
        auto pipe = make_pipe();
        if (!pipe) {
            return std::unexpected(RecordError::RemuxFailed);
        }
        std::expected<void, std::string> copied;
        std::jthread copier([&] {
            copied = deps_.remuxer.run(
                job(RecordingInput::FragmentedMp4, pipe->read.get()),
                [this, joined](std::string_view out) {
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
        if (!copied) {
            log("recording: copying epoch {} failed: {}", run.epoch, copied.error());
        }
        if (!poured) {
            return poured;
        }
        if (!copied) {
            return std::unexpected(stopped() ? RecordError::Stopped : RecordError::RemuxFailed);
        }
        return {};
    }

    std::expected<void, RecordError> pour_run(const RecordingRun& run, int sink) {
        const fs::path piece = settings_.work_dir / "piece";
        const auto init = key_in(settings_.stream, infra::ffmpeg::live_init_name(run.epoch));
        if (!init) {
            return std::unexpected(init.error());
        }
        if (auto poured = pour(deps_.store, *init, piece, sink, abort_.get_token()); !poured) {
            return poured;
        }
        for (std::uint64_t n = run.first; n <= run.last; ++n) {
            const auto segment =
                key_in(settings_.stream, infra::ffmpeg::live_segment_name(run.epoch, n));
            if (!segment) {
                return std::unexpected(segment.error());
            }
            if (auto poured = pour(deps_.store, *segment, piece, sink, abort_.get_token());
                !poured) {
                return poured;
            }
        }
        return {};
    }

    [[nodiscard]] bool stopped() const noexcept { return caller_stopped_.stop_requested(); }

    std::expected<std::uint64_t, RecordError> finish(bool joined) {
        if (stopped()) {
            return std::unexpected(RecordError::Stopped);
        }
        if (upload_error_) {
            log("recording: upload failed: {}", core::ports::to_string(*upload_error_));
            return std::unexpected(RecordError::UploadFailed);
        }
        if (!joined) {
            return std::unexpected(RecordError::RemuxFailed);
        }
        if (auto committed = output_.commit(); !committed) {
            log("recording: upload failed: {}", core::ports::to_string(committed.error()));
            return std::unexpected(RecordError::UploadFailed);
        }
        return bytes_;
    }

    const RecorderDeps& deps_;
    const RecorderSettings& settings_;
    core::ports::IObjectStream& output_;
    std::stop_source abort_;
    // Set only by the caller's stop; abort_ also fires on failures.
    std::stop_source caller_stopped_;
    std::stop_callback<Abort> on_stop_;
    std::optional<core::ports::StorageError> upload_error_;
    std::uint64_t bytes_ = 0;
};

std::expected<void, RecordError> assemble(const RecorderDeps& deps,
                                          const RecorderSettings& settings,
                                          const RecordingPlan& plan, const core::StorageKey& key,
                                          const std::stop_token& stop) {
    auto output = deps.streams.begin(key, recording_type());
    if (!output) {
        return std::unexpected(RecordError::UploadFailed);
    }
    Assembly assembly(deps, settings, **output, stop);
    const auto bytes = assembly.run(plan);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    std::uint64_t segments = 0;
    for (const RecordingRun& run : plan.runs) {
        segments += run.last - run.first + 1;
    }
    log("recording: {} segments in {} runs, {} missing, {} bytes", segments, plan.runs.size(),
        plan.missing, *bytes);
    return {};
}

RecordError from_plan(PlanError e) noexcept {
    switch (e) {
    case PlanError::StoreUnreadable:
        return RecordError::StoreUnreadable;
    case PlanError::NotEnded:
    case PlanError::Empty:
    case PlanError::PlaylistInvalid:
        return RecordError::PlaylistInvalid;
    }
    return RecordError::PlaylistInvalid;
}

} // namespace

std::string_view to_string(RecordError e) noexcept {
    switch (e) {
    case RecordError::PlaylistInvalid:
        return "stored playlist invalid";
    case RecordError::StoreUnreadable:
        return "store unreadable";
    case RecordError::RemuxFailed:
        return "remux failed";
    case RecordError::UploadFailed:
        return "upload failed";
    case RecordError::Stopped:
        return "stopped";
    case RecordError::DatabaseUnavailable:
        return "database unavailable";
    }
    return "unknown";
}

std::expected<std::optional<core::VideoId>, RecordError>
record_stream(const RecorderDeps& deps, const RecorderSettings& settings,
              const std::stop_token& stop) {
    const auto known = deps.recordings.find(settings.stream.str());
    if (!known) {
        return std::unexpected(RecordError::DatabaseUnavailable);
    }
    if (*known) {
        log("recording: already video {}", (*known)->to_string());
        return *known;
    }
    std::error_code ec;
    fs::create_directories(settings.work_dir, ec);
    if (ec) {
        return std::unexpected(RecordError::RemuxFailed);
    }
    const auto ended = read_ended(deps.store, settings.stream, settings.work_dir);
    if (!ended) {
        return std::unexpected(ended.error());
    }
    if (!*ended) {
        return std::nullopt;
    }
    const auto key = key_in(settings.stream, kRecordingName);
    if (!key) {
        return std::unexpected(key.error());
    }
    // A run that died after the commit and before the database left a whole recording.
    const auto existing = deps.store.size(*key);
    if (!existing) {
        if (existing.error() != core::ports::StorageError::NotFound) {
            return std::unexpected(RecordError::StoreUnreadable);
        }
        const auto plan = plan_recording(**ended, settings.stream, deps.store);
        if (!plan) {
            return std::unexpected(from_plan(plan.error()));
        }
        if (auto made = assemble(deps, settings, *plan, *key, stop); !made) {
            return std::unexpected(made.error());
        }
    }
    const auto video =
        deps.recordings.record({.stream = settings.stream.str(),
                                .video = core::VideoId::generate(deps.clock, deps.random),
                                .owner = settings.owner,
                                .title = "Live stream " + settings.stream.str(),
                                .source = *key});
    if (!video) {
        return std::unexpected(RecordError::DatabaseUnavailable);
    }
    log("recording: queued as video {}", video->to_string());
    return *video;
}

} // namespace live
