#pragma once

#include "core/models/ids.hpp"
#include "core/util/time.hpp"

#include "stream_id.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace live {

enum class StorageBackend : std::uint8_t { R2, Minio, Filesystem };

// Where the stream's recording is queued as a video once the stream ends (ADR-0055).
struct RecordingTarget {
    // A secret: it holds the database password, and is never logged.
    std::string database_url;
    // The user the stream belongs to, who owns the video it becomes.
    core::UserId owner;
};

struct Config {
    StreamId stream;
    // Where the publisher's SRT caller connects (UDP). A numeric address; loopback unless told
    // otherwise.
    std::string ingest_host;
    std::uint16_t ingest_port = 0;
    // What the caller must encrypt with. A secret: never logged.
    std::string srt_passphrase;
    StorageBackend storage = StorageBackend::R2;
    // The R2 account id, the MinIO endpoint URL, or the filesystem root.
    std::string storage_location;
    std::string bucket;
    // ULW_SCRATCH_DIR/<stream>: this packager's alone, which startup clears.
    std::filesystem::path scratch;
    // The ulw_sandbox helper, which runs the ffmpeg and ffprobe it was built with and no other
    // (docs/adr/0089); empty means the one installed beside this executable.
    std::filesystem::path sandbox;
    // PATH for the sandboxed children, which inherit nothing else. Nothing looks the programs
    // up in it.
    std::string search_path;
    std::uint32_t segment_seconds = 0;
    std::size_t window_segments = 0;
    core::Seconds max_duration{};
    // The most the publisher may send, in kbit/s; it bounds the size of a segment file.
    std::uint32_t max_kbps = 0;
    // Unset: the stream is live only, and nothing is recorded.
    std::optional<RecordingTarget> recording;
};

struct ConfigError {
    std::string variable;
    std::string reason;
};

using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Everything comes from the environment, as for the worker: arguments are visible to every
// user through /proc, and the storage keys are secrets.
[[nodiscard]] std::expected<Config, ConfigError> load_config(const EnvLookup& env);

// How many segments ffmpeg keeps in its own playlist: twice the window, so that an uploader
// that falls a whole window behind still finds every segment listed. Upload failures are given
// up on after one window (see stream_runner.cpp), before the list can move past a segment.
[[nodiscard]] constexpr std::uint32_t listed_segments(std::size_t window_segments) noexcept {
    return static_cast<std::uint32_t>(2 * window_segments);
}

} // namespace live
