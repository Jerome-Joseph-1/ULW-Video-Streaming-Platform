#include "config.hpp"

#include "core/util/parse.hpp"
#include "infra/ffmpeg/live_remux.hpp"
#include "infra/srt/ingest.hpp"

#include <utility>

namespace live {

namespace {

// Under /var/cache, which only root can write to, rather than a shared directory such as
// /var/tmp, where another user could make the directory first.
constexpr std::string_view kDefaultScratch = "/var/cache/ulw-live";
constexpr std::string_view kDefaultPath = "/usr/local/bin:/usr/bin:/bin";
constexpr std::string_view kDefaultIngestHost = "127.0.0.1";

// The playlist is rewritten once per segment and R2 takes one write per second per key
// (ADR-0014), so a segment must last more than a second. Past 10 s the stream is not live to
// its viewers: they sit three target durations behind the edge.
constexpr std::uint32_t kMinSegmentSeconds = 2;
constexpr std::uint32_t kMaxSegmentSeconds = infra::ffmpeg::kLiveMaxSegmentSeconds;
constexpr std::uint32_t kDefaultSegmentSeconds = 2;
// RFC 8216 section 6.2.2: a live playlist holds at least three target durations. Ten segments
// at the default 2 s keep 20 s, room for a viewer's stall or a slow reload. 64 keeps
// twice that (listed_segments) inside media_playlist.hpp's kMaxSegments.
constexpr std::size_t kMinWindow = 3;
constexpr std::size_t kMaxWindow = 64;
constexpr std::size_t kDefaultWindow = 10;
// The longest video the platform takes is 12 hours (infra/ffmpeg command.hpp); a live stream
// becomes one, so it is capped alike.
constexpr std::uint64_t kMaxHours = 12;
// The worker's ladder sends 5 Mbit/s for 1080p (brief 8.10); four times that covers a stream
// well above it, and past 100 the pipe, not the packager, is the limit.
constexpr std::uint32_t kMinKbps = 500;
constexpr std::uint32_t kDefaultKbps = 20'000;
constexpr std::uint32_t kMaxKbps = infra::ffmpeg::kLiveMaxKbps;

std::unexpected<ConfigError> error(std::string_view variable, std::string_view reason) {
    return std::unexpected(
        ConfigError{.variable = std::string(variable), .reason = std::string(reason)});
}

std::optional<std::string> lookup(const EnvLookup& env, std::string_view name) {
    auto value = env(name);
    if (value && value->empty()) {
        return std::nullopt;
    }
    return value;
}

std::expected<std::string, ConfigError> required(const EnvLookup& env, std::string_view name) {
    auto value = lookup(env, name);
    if (!value) {
        return error(name, "not set");
    }
    return std::move(*value);
}

std::expected<std::filesystem::path, ConfigError>
absolute_path(const EnvLookup& env, std::string_view name, std::string_view fallback) {
    std::filesystem::path p = lookup(env, name).value_or(std::string(fallback));
    if (!p.empty() && !p.is_absolute()) {
        return error(name, "must be an absolute path");
    }
    return p;
}

template <class T>
std::expected<T, ConfigError> bounded(const EnvLookup& env, std::string_view name, T fallback,
                                      T min, T max) {
    const auto text = lookup(env, name);
    if (!text) {
        return fallback;
    }
    const auto value = core::parse_integer<T>(*text);
    if (!value || *value < min || *value > max) {
        return error(name, "not an integer in " + std::to_string(min) + ".." + std::to_string(max));
    }
    return *value;
}

struct Storage {
    StorageBackend backend;
    std::string location;
    std::string bucket;
};

std::expected<Storage, ConfigError> load_storage(const EnvLookup& env) {
    const std::string kind = lookup(env, "ULW_STORAGE").value_or("r2");
    Storage storage{.backend = StorageBackend::R2, .location = {}, .bucket = {}};
    std::string_view location_variable;
    if (kind == "r2") {
        location_variable = "ULW_R2_ACCOUNT_ID";
    } else if (kind == "minio") {
        storage.backend = StorageBackend::Minio;
        location_variable = "ULW_S3_ENDPOINT";
    } else if (kind == "fs") {
        storage.backend = StorageBackend::Filesystem;
        location_variable = "ULW_FS_ROOT";
    } else {
        return error("ULW_STORAGE", "expected r2, minio or fs");
    }
    auto location = required(env, location_variable);
    if (!location) {
        return std::unexpected(std::move(location.error()));
    }
    storage.location = std::move(*location);
    if (storage.backend == StorageBackend::Filesystem) {
        return storage;
    }
    auto bucket = required(env, "ULW_BUCKET");
    if (!bucket) {
        return std::unexpected(std::move(bucket.error()));
    }
    storage.bucket = std::move(*bucket);
    return storage;
}

// Both or neither: a database without an owner, or the reverse, is a half-configured recorder.
std::expected<std::optional<RecordingTarget>, ConfigError> load_recording(const EnvLookup& env) {
    auto url = lookup(env, "ULW_DATABASE_URL");
    const auto owner_text = lookup(env, "ULW_STREAM_OWNER");
    if (!url && !owner_text) {
        return std::nullopt;
    }
    if (!url) {
        return error("ULW_DATABASE_URL", "not set, and ULW_STREAM_OWNER is");
    }
    if (!owner_text) {
        return error("ULW_STREAM_OWNER", "not set, and ULW_DATABASE_URL is");
    }
    const auto owner = core::UserId::parse(*owner_text);
    if (!owner) {
        return error("ULW_STREAM_OWNER", "not a user id");
    }
    return RecordingTarget{.database_url = std::move(*url), .owner = *owner};
}

} // namespace

std::expected<Config, ConfigError> load_config(const EnvLookup& env) {
    auto stream_text = required(env, "ULW_STREAM_ID");
    if (!stream_text) {
        return std::unexpected(std::move(stream_text.error()));
    }
    auto stream = StreamId::parse(*stream_text);
    if (!stream) {
        return error("ULW_STREAM_ID", stream.error());
    }
    auto port_text = required(env, "ULW_LIVE_INGEST_PORT");
    if (!port_text) {
        return std::unexpected(std::move(port_text.error()));
    }
    const auto port = core::parse_integer<std::uint16_t>(*port_text);
    if (!port) {
        return error("ULW_LIVE_INGEST_PORT", "not a port number");
    }
    auto passphrase = required(env, "ULW_LIVE_SRT_PASSPHRASE");
    if (!passphrase) {
        return std::unexpected(std::move(passphrase.error()));
    }
    if (passphrase->size() < infra::srt::kMinPassphrase ||
        passphrase->size() > infra::srt::kMaxPassphrase) {
        return error("ULW_LIVE_SRT_PASSPHRASE", "not 10 to 79 characters");
    }
    auto storage = load_storage(env);
    if (!storage) {
        return std::unexpected(std::move(storage.error()));
    }
    auto scratch = absolute_path(env, "ULW_SCRATCH_DIR", kDefaultScratch);
    if (!scratch) {
        return std::unexpected(std::move(scratch.error()));
    }
    auto sandbox = absolute_path(env, "ULW_SANDBOX_BIN", "");
    if (!sandbox) {
        return std::unexpected(std::move(sandbox.error()));
    }
    const auto segment =
        bounded<std::uint32_t>(env, "ULW_LIVE_SEGMENT_SECONDS", kDefaultSegmentSeconds,
                               kMinSegmentSeconds, kMaxSegmentSeconds);
    if (!segment) {
        return std::unexpected(segment.error());
    }
    const auto window = bounded<std::size_t>(env, "ULW_LIVE_WINDOW_SEGMENTS", kDefaultWindow,
                                             kMinWindow, kMaxWindow);
    if (!window) {
        return std::unexpected(window.error());
    }
    const auto hours = bounded<std::uint64_t>(env, "ULW_LIVE_MAX_HOURS", kMaxHours, 1, kMaxHours);
    if (!hours) {
        return std::unexpected(hours.error());
    }
    const auto kbps =
        bounded<std::uint32_t>(env, "ULW_LIVE_MAX_KBPS", kDefaultKbps, kMinKbps, kMaxKbps);
    if (!kbps) {
        return std::unexpected(kbps.error());
    }
    auto recording = load_recording(env);
    if (!recording) {
        return std::unexpected(std::move(recording.error()));
    }
    return Config{.stream = std::move(*stream),
                  .ingest_host =
                      lookup(env, "ULW_LIVE_INGEST_HOST").value_or(std::string(kDefaultIngestHost)),
                  .ingest_port = *port,
                  .srt_passphrase = std::move(*passphrase),
                  .storage = storage->backend,
                  .storage_location = std::move(storage->location),
                  .bucket = std::move(storage->bucket),
                  // Startup clears it, so two streams given one scratch root, as by the
                  // default, must not clear each other's.
                  .scratch = *scratch / *stream_text,
                  .sandbox = std::move(*sandbox),
                  .ffmpeg = lookup(env, "ULW_FFMPEG").value_or("ffmpeg"),
                  .ffprobe = lookup(env, "ULW_FFPROBE").value_or("ffprobe"),
                  .search_path = lookup(env, "PATH").value_or(std::string(kDefaultPath)),
                  .segment_seconds = *segment,
                  .window_segments = *window,
                  .max_duration = core::Seconds{static_cast<std::int64_t>(*hours) * 3600},
                  .max_kbps = *kbps,
                  .recording = std::move(*recording)};
}

} // namespace live
