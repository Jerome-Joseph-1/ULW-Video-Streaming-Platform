#pragma once

#include "core/models/ladder.hpp"
#include "core/ports/transcoder.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace infra::ffmpeg {

// Every segment is this long, and every rung has a keyframe at each multiple of it, so a
// player can switch rungs at any segment boundary.
inline constexpr std::uint32_t kSegmentSeconds = 4;

// Frames per segment: round(fps x 4), computed on the rational so 30000/1001 gives 120.
[[nodiscard]] std::uint32_t gop_frames(core::ports::FrameRate rate) noexcept;

// argv, program name first.
using Args = std::vector<std::string>;

// The demuxers an upload may be read with. Left to probe, ffmpeg picks among all of them, and
// playlist and manifest formats (DASH, HLS, IMF, concat) open whatever local paths the upload
// names, outside the workspace too. These are the containers cameras, phones and editors
// write: MP4 and QuickTime (mov), MKV and WebM, MPEG-TS, AVI, FLV, WMV (asf), MPEG-PS and Ogg.
inline constexpr std::string_view kSourceFormats = "mov,matroska,mpegts,avi,flv,asf,mpeg,ogg";
// What verification reads of our own output: the HLS playlists and their fMP4 segments.
inline constexpr std::string_view kOutputFormats = "hls,mov";

// The longest video we take. There is no longer one in the product brief; twelve hours is what
// the largest video sites allow in one upload, and the 50 GiB upload cap
// (core::Upload::kMaxSizeBytes) holds twelve hours at 9.9 Mbit/s. The transcode budgets scale
// with the duration, so this also bounds how long one attempt may hold a worker.
inline constexpr core::Millis kMaxDuration{12LL * 3600 * 1000};
// A duration comes from the upload's own headers. The sparsest real video, a still 240p frame
// at 1 fps in a single GOP, came to 230 bit/s through x264 into MP4 over ten minutes; a file
// with fewer bits than this for each second it declares cannot hold what it declares.
inline constexpr std::uint64_t kMinSourceBitsPerSecond = 128;

[[nodiscard]] Args probe_args(const std::string& ffprobe, const std::filesystem::path& input);
// What probe_args prints for a file of `source_bytes`, or why it describes nothing that can be
// transcoded.
[[nodiscard]] std::expected<core::ports::MediaInfo, std::string>
parse_probe(std::string_view text, std::uint64_t source_bytes);

[[nodiscard]] Args transcode_args(const std::string& ffmpeg, const std::filesystem::path& input,
                                  const std::filesystem::path& out_dir,
                                  const core::ports::MediaInfo& media,
                                  std::span<const core::Rung> ladder, unsigned threads);

[[nodiscard]] Args keyframe_args(const std::string& ffprobe, const std::filesystem::path& playlist);
// The pts_time of every keyframe, in order; nullopt when a line is not a time.
[[nodiscard]] std::optional<std::vector<std::string>> parse_keyframes(std::string_view text);

[[nodiscard]] Args decode_args(const std::string& ffmpeg, const std::filesystem::path& master);

// The master playlist with each variant's BANDWIDTH set from the ladder, as ffmpeg 6.1 wrote it
// (the rung's video rate, plus the audio rate when there is audio, plus a tenth), and without
// AVERAGE-BANDWIDTH. ffmpeg 7 rewrites the master when it finishes with the peak and average
// bitrate it measured over the segments, which differ from run to run; everything but the
// segments must be the same for every run of a job, since a rerun may overwrite some of another
// run's keys. A variant whose URI names no rung is left as it is, for the check to report.
[[nodiscard]] std::string
settle_master_bandwidth(std::string_view text, std::span<const core::Rung> ladder, bool has_audio);

// What is wrong with a playlist ffmpeg wrote, or nullopt when nothing is.
[[nodiscard]] std::optional<std::string> check_media_playlist(std::string_view text);
// A master must also state the ladder's rates, as settle_master_bandwidth writes them, so an
// ffmpeg whose master that no longer settles fails verification rather than shipping.
[[nodiscard]] std::optional<std::string>
check_master_playlist(std::string_view text, std::span<const core::Rung> ladder, bool has_audio);

} // namespace infra::ffmpeg
