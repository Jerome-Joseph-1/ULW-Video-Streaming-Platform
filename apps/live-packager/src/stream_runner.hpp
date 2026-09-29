#pragma once

#include "core/ports/clock.hpp"
#include "infra/ffmpeg/live_remux.hpp"
#include "infra/srt/ingest.hpp"

#include "publisher.hpp"

#include <cstdint>
#include <filesystem>
#include <stop_token>

namespace live {

enum class Outcome : std::uint8_t {
    // The stream ended (the publisher finished, or the end was asked for), or the process was
    // drained and the stream left to be continued.
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
    std::uint32_t max_kbps = 0;
    core::Seconds max_duration{};
};

struct StopRequests {
    // SIGTERM: this process is going away, and the stream is left as it is, to be continued by
    // the next process started for it. Nothing is ended, since the stream is not.
    std::stop_token drain;
    // SIGUSR1: the stream is over. Its playlist gets EXT-X-ENDLIST.
    std::stop_token end;
};

// One stream, start to end: waits for the publisher, hands what it sends to ffmpeg, uploads
// each segment as ffmpeg finishes it, and ends the playlist when the stream ends.
[[nodiscard]] Outcome run_stream(Publisher& publisher, infra::srt::IngestListener& listener,
                                 const infra::ffmpeg::LiveRemuxer& remuxer,
                                 const core::ports::IClock& clock, const RunSettings& settings,
                                 const StopRequests& stops);

} // namespace live
