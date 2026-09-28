#include "core/models/ladder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

namespace {

struct Step {
    std::uint32_t height;
    std::uint32_t video_kbps;
};

constexpr std::array kSteps{Step{.height = 1080, .video_kbps = 5000},
                            Step{.height = 720, .video_kbps = 2800},
                            Step{.height = 360, .video_kbps = 800}};

std::string name_of(std::uint32_t height) {
    return std::to_string(height) + "p";
}

} // namespace

std::vector<Rung> choose_ladder(std::uint32_t source_height) {
    std::vector<Rung> ladder;
    for (const Step& step : kSteps) {
        if (source_height >= step.height) {
            ladder.push_back({.name = name_of(step.height),
                              .height = step.height,
                              .video_kbps = step.video_kbps});
        }
    }
    if (!ladder.empty()) {
        return ladder;
    }
    // H.264 in 4:2:0 needs an even height. The bitrate shrinks with the height from the
    // 360p rung's, which keeps bits per line about the same.
    const Step& smallest = kSteps.back();
    const std::uint32_t height = std::max<std::uint32_t>(2, source_height & ~std::uint32_t{1});
    const auto kbps =
        static_cast<std::uint32_t>(std::uint64_t{smallest.video_kbps} * height / smallest.height);
    ladder.push_back({.name = name_of(height), .height = height, .video_kbps = kbps});
    return ladder;
}

} // namespace core
