#include "progress.hpp"

#include "core/util/parse.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace infra::ffmpeg {

namespace {

// Progress lines are under 40 bytes; anything longer is not one and is skipped whole, which
// also bounds what a misbehaving child can make us buffer.
constexpr std::size_t kMaxLine = 256;

} // namespace

std::optional<core::Millis> ProgressParser::feed(std::string_view bytes) {
    completed_.reset();
    while (!bytes.empty()) {
        const std::size_t eol = bytes.find('\n');
        const std::string_view piece = bytes.substr(0, eol);
        if (partial_.size() + piece.size() > kMaxLine) {
            overlong_ = true;
            partial_.clear();
        } else if (!overlong_) {
            partial_.append(piece);
        }
        if (eol == std::string_view::npos) {
            break;
        }
        bytes.remove_prefix(eol + 1);
        if (!overlong_) {
            take_line(partial_);
        }
        partial_.clear();
        overlong_ = false;
    }
    return std::exchange(completed_, std::nullopt);
}

void ProgressParser::take_line(std::string_view line) {
    if (line.ends_with('\r')) {
        line.remove_suffix(1);
    }
    constexpr std::string_view kTime = "out_time_us=";
    constexpr std::string_view kProgress = "progress=";
    if (line.starts_with(kTime)) {
        // "N/A" until the first frame is out; a negative value before the first timestamp.
        if (const auto us = core::parse_integer<std::uint64_t>(line.substr(kTime.size()))) {
            pending_ = core::Millis{static_cast<std::int64_t>(*us / 1000)};
        }
    } else if (line.starts_with(kProgress)) {
        if (pending_) {
            completed_ = pending_;
        }
        pending_.reset();
        ended_ = ended_ || line.substr(kProgress.size()) == "end";
    }
}

} // namespace infra::ffmpeg
