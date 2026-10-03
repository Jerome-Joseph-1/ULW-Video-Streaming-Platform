#include "infra/ffmpeg/transcoder.hpp"

#include "command.hpp"
#include "exit_code.hpp"
#include "process.hpp"
#include "progress.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace infra::ffmpeg {

namespace {

namespace fs = std::filesystem;
using core::ports::MediaInfo;
using core::ports::TranscodeError;
using core::ports::TranscodeFailure;
using core::ports::TranscodeResult;

constexpr std::uint64_t kGiB = std::uint64_t{1} << 30U;

// ffprobe reads headers and never decodes a frame; a minute of either is a stuck prober.
Limits probe_budget(const fs::path& writable) {
    return {.writable = writable,
            .address_space_bytes = kGiB,
            .cpu = core::Seconds{60},
            .wall = core::Millis{60'000}};
}

// A 60 s 1080p source through the three-rung ladder with -threads 4 peaked at 1.9 GB of
// address space (556 MB resident); 4 GiB leaves twice that.
constexpr std::uint64_t kTranscodeAddressSpace = 4 * kGiB;
// Measured on a 4-core box shared with other builds: 0.73x realtime and 1.46 CPU-seconds per
// second of media for that ladder. The wall budget tolerates 0.25x realtime and the CPU budget
// 8x the measured cost; the floors cover start-up and very short clips.
constexpr core::Millis kBudgetFloor{10 * 60'000};
constexpr std::int64_t kWallPerMedia = 4;
constexpr std::int64_t kCpuPerMedia = 12;

// ffprobe's answers are a few hundred bytes; a keyframe list is a dozen bytes a segment.
constexpr std::size_t kMaxProbeOutput = std::size_t{1} << 20U;
constexpr std::size_t kMaxKeyframeOutput = std::size_t{8} << 20U;
// A media playlist is about 40 bytes a segment: two hours of 4 s segments is 72 KB.
constexpr std::size_t kMaxPlaylist = std::size_t{4} << 20U;

Limits transcode_budget(const MediaInfo& media, const fs::path& writable) {
    const auto wall = kBudgetFloor + media.duration * kWallPerMedia;
    const auto cpu = kBudgetFloor + media.duration * kCpuPerMedia;
    return {.writable = writable,
            .address_space_bytes = kTranscodeAddressSpace,
            .cpu = std::chrono::duration_cast<core::Seconds>(cpu),
            .wall = wall};
}

std::string last_line(std::string_view text) {
    while (text.ends_with('\n') || text.ends_with('\r')) {
        text.remove_suffix(1);
    }
    const std::size_t nl = text.rfind('\n');
    return std::string(nl == std::string_view::npos ? text : text.substr(nl + 1));
}

TranscodeError error_of(TranscodeFailure kind, const ChildExit& child, std::string_view what) {
    const std::string why = last_line(child.stderr_tail);
    return {.kind = kind,
            .exit_code = child.exit_code,
            .detail = std::string(what) +
                      (child.signal != 0 ? " killed by signal " + std::to_string(child.signal)
                                         : " exited " + std::to_string(child.exit_code)) +
                      (why.empty() ? "" : ": " + why)};
}

TranscodeError spawn_error(std::string detail) {
    return {.kind = TranscodeFailure::Sandbox, .exit_code = -1, .detail = std::move(detail)};
}

TranscodeError unverified(std::string detail) {
    return {.kind = TranscodeFailure::Unverified, .exit_code = 0, .detail = std::move(detail)};
}

// Collects stdout up to `max`; past it, the rest is dropped and the parse that follows fails.
auto collect_into(std::string& out, std::size_t max) {
    return [&out, max](std::string_view bytes) {
        if (out.size() + bytes.size() <= max) {
            out.append(bytes);
        }
    };
}

std::optional<std::string> read_text(const fs::path& p) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec || size > kMaxPlaylist) {
        return std::nullopt;
    }
    std::ifstream in(p, std::ios::binary);
    std::string text(size, '\0');
    in.read(text.data(), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        return std::nullopt;
    }
    return text;
}

// Replaces a file's contents by renaming a complete copy over it.
[[nodiscard]] bool replace_text(const fs::path& p, std::string_view text) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        // Closing flushes; a write that fails there (a full disk) shows only on the stream.
        out.close();
        if (!out) {
            std::error_code ignored;
            fs::remove(tmp, ignored);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

} // namespace

FfmpegTranscoder::FfmpegTranscoder(TranscoderConfig config, const core::ports::IClock& clock)
    : config_(std::move(config)), clock_(clock) {}

TranscodeResult<MediaInfo> FfmpegTranscoder::probe(const fs::path& input, std::stop_token stop) {
    std::error_code ec;
    const std::uint64_t source_bytes = fs::file_size(input, ec);
    if (ec) {
        // The worker fetched the source a moment ago: not being able to stat it is ours.
        return std::unexpected(TranscodeError{.kind = TranscodeFailure::Inaccessible,
                                              .exit_code = 0,
                                              .detail = "source: " + ec.message()});
    }
    const Sandbox sandbox{.helper = config_.sandbox,
                          .environment = {"PATH=" + config_.search_path}};
    std::string output;
    auto child = run_sandboxed(sandbox, probe_budget(input.parent_path()),
                               probe_args(config_.ffprobe, input), clock_,
                               collect_into(output, kMaxProbeOutput), stop);
    if (!child) {
        return std::unexpected(spawn_error(std::move(child.error())));
    }
    if (const auto failure = classify(child->exit_code, child->signal, child->ending)) {
        const std::array ours{input};
        return std::unexpected(error_of(
            refine(*failure, child->exit_code, child->stderr_tail, ours), *child, "ffprobe"));
    }
    auto media = parse_probe(output, source_bytes);
    if (!media) {
        return std::unexpected(TranscodeError{.kind = TranscodeFailure::Rejected,
                                              .exit_code = 0,
                                              .detail = std::move(media.error())});
    }
    return *media;
}

TranscodeResult<core::ports::TranscodeStats>
FfmpegTranscoder::run(const fs::path& input, const fs::path& out_dir, const MediaInfo& media,
                      std::span<const core::Rung> ladder, core::ports::ITranscodeProgress& progress,
                      std::stop_token stop) {
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        return std::unexpected(spawn_error("create " + out_dir.string() + ": " + ec.message()));
    }
    const Sandbox sandbox{.helper = config_.sandbox,
                          .environment = {"PATH=" + config_.search_path}};
    ProgressParser parser;
    const auto on_stdout = [&](std::string_view bytes) {
        if (const auto encoded = parser.feed(bytes)) {
            progress.on_progress(*encoded);
        }
    };
    auto child = run_sandboxed(
        sandbox, transcode_budget(media, out_dir),
        transcode_args(config_.ffmpeg, input, out_dir, media, ladder, config_.threads), clock_,
        on_stdout, stop);
    if (!child) {
        return std::unexpected(spawn_error(std::move(child.error())));
    }
    if (const auto failure = classify(child->exit_code, child->signal, child->ending)) {
        const std::array ours{input, out_dir};
        return std::unexpected(error_of(
            refine(*failure, child->exit_code, child->stderr_tail, ours), *child, "ffmpeg"));
    }
    // The rates ffmpeg 7 puts in the master differ from run to run (settle_master_bandwidth).
    // A master that is missing or unreadable is left for verify to report.
    const fs::path master = out_dir / "master.m3u8";
    if (const auto text = read_text(master);
        text && !replace_text(master, settle_master_bandwidth(*text, ladder, media.has_audio))) {
        return std::unexpected(unverified("master playlist could not be rewritten"));
    }
    return core::ports::TranscodeStats{.wall = child->wall, .peak_rss_kib = child->peak_rss_kib};
}

TranscodeResult<void> FfmpegTranscoder::verify(const fs::path& out_dir, const MediaInfo& media,
                                               std::span<const core::Rung> ladder,
                                               std::stop_token stop) {
    const auto master_text = read_text(out_dir / "master.m3u8");
    if (!master_text) {
        return std::unexpected(unverified("master playlist unreadable"));
    }
    if (auto problem = check_master_playlist(*master_text, ladder, media.has_audio)) {
        return std::unexpected(unverified(std::move(*problem)));
    }
    for (const core::Rung& rung : ladder) {
        const auto text = read_text(out_dir / rung.name / "index.m3u8");
        if (!text) {
            return std::unexpected(unverified(rung.name + " playlist unreadable"));
        }
        if (auto problem = check_media_playlist(*text)) {
            return std::unexpected(unverified(rung.name + ": " + *problem));
        }
    }

    const Sandbox sandbox{.helper = config_.sandbox,
                          .environment = {"PATH=" + config_.search_path}};
    const Limits limits = transcode_budget(media, out_dir);
    // What a checking child's failure means: its input is our output, so anything it refuses
    // is our output failing verification.
    // `read` is what the checking child was given to open.
    const auto failed_check = [](const ChildExit& child, std::string_view what,
                                 const fs::path& read) {
        const std::array ours{read};
        auto kind = refine(classify(child.exit_code, child.signal, child.ending)
                               .value_or(TranscodeFailure::Unverified),
                           child.exit_code, child.stderr_tail, ours);
        if (kind == TranscodeFailure::Rejected) {
            kind = TranscodeFailure::Unverified;
        }
        return error_of(kind, child, what);
    };

    std::optional<std::vector<std::string>> reference;
    for (const core::Rung& rung : ladder) {
        std::string output;
        auto child = run_sandboxed(
            sandbox, limits, keyframe_args(config_.ffprobe, out_dir / rung.name / "index.m3u8"),
            clock_, collect_into(output, kMaxKeyframeOutput), stop);
        if (!child) {
            return std::unexpected(spawn_error(std::move(child.error())));
        }
        if (child->exit_code != 0 || child->ending != Ending::Exited) {
            return std::unexpected(
                failed_check(*child, "ffprobe " + rung.name, out_dir / rung.name / "index.m3u8"));
        }
        auto keyframes = parse_keyframes(output);
        if (!keyframes || keyframes->empty()) {
            return std::unexpected(unverified(rung.name + " has no readable keyframes"));
        }
        if (!reference) {
            reference = std::move(keyframes);
        } else if (*keyframes != *reference) {
            return std::unexpected(
                unverified(rung.name + " keyframes are not aligned with " + ladder.front().name));
        }
    }

    auto child = run_sandboxed(
        sandbox, limits, decode_args(config_.ffmpeg, out_dir / "master.m3u8"), clock_,
        [](std::string_view) {}, stop);
    if (!child) {
        return std::unexpected(spawn_error(std::move(child.error())));
    }
    if (child->exit_code != 0 || child->ending != Ending::Exited) {
        return std::unexpected(failed_check(*child, "decode check", out_dir / "master.m3u8"));
    }
    // -v error prints nothing at all for a clean decode.
    if (child->stderr_bytes != 0) {
        return std::unexpected(unverified("decode check: " + last_line(child->stderr_tail)));
    }
    return {};
}

std::optional<std::string> check_sandbox(const fs::path& helper, const fs::path& writable,
                                         const core::ports::IClock& clock) {
    auto child = run_sandboxed(Sandbox{.helper = helper, .environment = {}}, probe_budget(writable),
                               {}, clock, [](std::string_view) {}, {});
    if (!child) {
        return std::move(child.error());
    }
    if (child->exit_code != 0) {
        const std::string why = last_line(child->stderr_tail);
        return why.empty() ? "sandbox helper exited " + std::to_string(child->exit_code) : why;
    }
    return std::nullopt;
}

} // namespace infra::ffmpeg
