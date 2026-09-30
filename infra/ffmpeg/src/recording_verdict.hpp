#pragma once

#include "infra/ffmpeg/recording_remux.hpp"

#include "process.hpp"

#include <expected>
#include <string_view>

namespace infra::ffmpeg {

// What a copying or probing child's exit says: only an error status of ffmpeg's own (classify's
// Rejected) is a verdict on the input.
[[nodiscard]] std::expected<void, RemuxError> recording_verdict(const ChildExit& child,
                                                                std::string_view program);

} // namespace infra::ffmpeg
