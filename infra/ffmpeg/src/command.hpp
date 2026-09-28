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

[[nodiscard]] Args probe_args(const std::string& ffprobe, const std::filesystem::path& input);
// What probe_args prints, or why it describes nothing that can be transcoded.
[[nodiscard]] std::expected<core::ports::MediaInfo, std::string> parse_probe(std::string_view text);

[[nodiscard]] Args transcode_args(const std::string& ffmpeg, const std::filesystem::path& input,
                                  const std::filesystem::path& out_dir,
                                  const core::ports::MediaInfo& media,
                                  std::span<const core::Rung> ladder, unsigned threads);

[[nodiscard]] Args keyframe_args(const std::string& ffprobe, const std::filesystem::path& playlist);
// The pts_time of every keyframe, in order; nullopt when a line is not a time.
[[nodiscard]] std::optional<std::vector<std::string>> parse_keyframes(std::string_view text);

[[nodiscard]] Args decode_args(const std::string& ffmpeg, const std::filesystem::path& master);

// What is wrong with a playlist ffmpeg wrote, or nullopt when nothing is.
[[nodiscard]] std::optional<std::string> check_media_playlist(std::string_view text);
[[nodiscard]] std::optional<std::string> check_master_playlist(std::string_view text,
                                                               std::span<const core::Rung> ladder);

} // namespace infra::ffmpeg
