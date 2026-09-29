#pragma once

#include "infra/ffmpeg/live_remux.hpp"

#include "command.hpp"

namespace infra::ffmpeg {

// argv, program name first.
[[nodiscard]] Args live_remux_args(const std::string& ffmpeg, const LiveRemuxJob& job);

} // namespace infra::ffmpeg
