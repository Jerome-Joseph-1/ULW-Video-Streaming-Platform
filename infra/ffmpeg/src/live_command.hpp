#pragma once

#include "infra/ffmpeg/live_remux.hpp"

#include "command.hpp"

#include <string>
#include <string_view>

namespace infra::ffmpeg {

// argv, program name first, probing as `probe` says (live_probe).
[[nodiscard]] Args live_remux_args(const std::string& ffmpeg, const LiveRemuxJob& job,
                                   const LiveProbe& probe);

// Whether ffmpeg's stderr says it found no codec parameters for a video stream: the line
// "Could not find codec parameters for stream N (Video: ...)" that it prints when the probe
// window ends before the first keyframe.
[[nodiscard]] bool video_unprobed(std::string_view stderr_text) noexcept;

} // namespace infra::ffmpeg
