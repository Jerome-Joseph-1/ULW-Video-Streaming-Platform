#pragma once

#include "core/ports/clock.hpp"
#include "infra/ffmpeg/live_remux.hpp"

#include "ingest.hpp"
#include "publisher.hpp"

#include <cstdint>
#include <filesystem>
#include <stop_token>

namespace live {

enum class Outcome : std::uint8_t {
    // The publisher finished, or the packager was told to stop: the stream is ended.
    Ended,
    // The stream broke: the store, the remuxer or the input failed. An ENDLIST was still
    // attempted, so viewers do not wait on a playlist that will never grow.
    Failed,
};

struct RunSettings {
    // ffmpeg's output directory, the only one it may write.
    std::filesystem::path media_dir;
    std::uint32_t segment_seconds = 0;
    std::uint32_t listed_segments = 0;
    core::Seconds max_duration{};
};

// One stream, start to end: waits for the publisher, remuxes what it sends, uploads each
// segment as ffmpeg finishes it, and ends the playlist when the stream stops.
[[nodiscard]] Outcome run_stream(Publisher& publisher, IngestListener& listener,
                                 const infra::ffmpeg::LiveRemuxer& remuxer,
                                 const core::ports::IClock& clock, const RunSettings& settings,
                                 const std::stop_token& stop);

} // namespace live
