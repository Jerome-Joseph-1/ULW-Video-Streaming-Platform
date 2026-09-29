#pragma once

#include "media_playlist.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace live {

struct CompletedSegment {
    std::uint64_t sequence = 0;
    std::string uri;
    Micros duration{};
    std::string init;
};

enum class ScanError : std::uint8_t {
    // The playlist text does not parse: caught half-written, or not a playlist.
    Unreadable,
    // The playlist ends before a segment already handed out, or names a segment other than the
    // one its sequence number says: this is not the run being tracked.
    Inconsistent,
    // The playlist has moved past segments never handed out: ffmpeg's list is finite, so the
    // uploader fell that far behind.
    Lost,
};

// Bytes of a file in ffmpeg's output directory, or nullopt when it is not there.
using FileSize = std::function<std::optional<std::uint64_t>(std::string_view name)>;

// Tells which segments ffmpeg has finished. The source of truth is ffmpeg's own playlist, which
// lists a segment only once the file is closed; a listed file that is empty or missing anyway
// (a rename not yet visible, a disk that lost it) is not complete, and holds back the segments
// after it so they are handed out in order.
class SegmentTracker {
public:
    SegmentTracker(std::uint32_t epoch, std::uint64_t first_sequence) noexcept
        : epoch_(epoch), next_(first_sequence) {}

    // The listed segments not yet handed out, in order, that are complete. Hands out nothing
    // twice, but does not mark anything handed out: see `handled`.
    [[nodiscard]] std::expected<std::vector<CompletedSegment>, ScanError>
    scan(std::string_view playlist_text, const FileSize& size_of) const;

    // Segments up to and including `sequence` are done with.
    void handled(std::uint64_t sequence) noexcept { next_ = sequence + 1; }
    [[nodiscard]] std::uint64_t next() const noexcept { return next_; }

private:
    std::uint32_t epoch_;
    std::uint64_t next_;
};

// One past the newest sequence number ffmpeg's playlist lists, or nullopt when the text is not
// a playlist (caught half-written). It moves when ffmpeg finishes a segment, whatever became of
// the upload, which is what tells a publisher that keeps sending from a store that keeps
// failing.
[[nodiscard]] std::optional<std::uint64_t> listed_end(std::string_view playlist_text);

} // namespace live
