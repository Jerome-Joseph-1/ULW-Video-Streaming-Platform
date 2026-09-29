#include "segment_tracker.hpp"

#include "infra/ffmpeg/live_remux.hpp"

namespace live {

std::expected<std::vector<CompletedSegment>, ScanError>
SegmentTracker::scan(std::string_view playlist_text, const FileSize& size_of) const {
    const auto playlist = parse_media_playlist(playlist_text);
    if (!playlist) {
        return std::unexpected(ScanError::Unreadable);
    }
    const std::uint64_t end = playlist->media_sequence + playlist->segments.size();
    if (end < next_) {
        return std::unexpected(ScanError::Inconsistent);
    }
    if (playlist->media_sequence > next_) {
        return std::unexpected(ScanError::Lost);
    }
    std::vector<CompletedSegment> ready;
    for (std::uint64_t sequence = next_; sequence < end; ++sequence) {
        const Segment& listed = playlist->segments[sequence - playlist->media_sequence];
        if (listed.uri != infra::ffmpeg::live_segment_name(epoch_, sequence)) {
            return std::unexpected(ScanError::Inconsistent);
        }
        const auto size = size_of(listed.uri);
        if (!size || *size == 0) {
            break;
        }
        ready.push_back({.sequence = sequence,
                         .uri = listed.uri,
                         .duration = listed.duration,
                         .init = listed.init});
    }
    return ready;
}

std::optional<std::uint64_t> listed_end(std::string_view playlist_text) {
    const auto playlist = parse_media_playlist(playlist_text);
    if (!playlist) {
        return std::nullopt;
    }
    return playlist->media_sequence + playlist->segments.size();
}

} // namespace live
