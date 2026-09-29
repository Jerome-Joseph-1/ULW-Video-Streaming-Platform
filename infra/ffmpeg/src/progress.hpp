#pragma once

#include "core/util/time.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace infra::ffmpeg {

// Reads ffmpeg's `-progress pipe:1` stream: blocks of key=value lines, each ended by a
// "progress=continue" or "progress=end" line. Bytes may arrive split anywhere.
class ProgressParser {
public:
    // Returns the encoded position when `bytes` completed at least one block that had one.
    [[nodiscard]] std::optional<core::Millis> feed(std::string_view bytes);
    [[nodiscard]] bool ended() const noexcept { return ended_; }

private:
    void take_line(std::string_view line);

    std::string partial_;
    bool overlong_ = false;
    std::optional<core::Millis> pending_;
    std::optional<core::Millis> completed_;
    bool ended_ = false;
};

} // namespace infra::ffmpeg
