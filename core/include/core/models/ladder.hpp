#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

// One output of the HLS ladder. `name` is its directory and its var_stream_map name.
struct Rung {
    std::string name;
    std::uint32_t height = 0;
    std::uint32_t video_kbps = 0;

    friend bool operator==(const Rung&, const Rung&) = default;
};

// The audio every rung carries, when the source has any.
inline constexpr std::uint32_t kAudioKbps = 128;

// 1080p, 720p and 360p, tallest first, each only when the source is at least that tall, so
// nothing is ever upscaled. A source shorter than 360 lines gets a single rung at its own
// height.
[[nodiscard]] std::vector<Rung> choose_ladder(std::uint32_t source_height);

} // namespace core
