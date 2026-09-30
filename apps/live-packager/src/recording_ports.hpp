#pragma once

#include "infra/ffmpeg/recording_remux.hpp"
#include "infra/postgres/live_recordings.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string_view>

namespace live {

// Where a stream's end becomes a video and a job, or a mark that it cannot. The database's
// adapter in production (PgLiveRecordings); a fake in the recorder's tests.
class IRecordingCatalog {
public:
    virtual ~IRecordingCatalog() = default;
    [[nodiscard]] virtual std::expected<std::optional<infra::postgres::RecordingRow>,
                                        infra::postgres::RecordingStoreError>
    find(std::string_view stream) = 0;
    [[nodiscard]] virtual std::expected<infra::postgres::RecordingRow,
                                        infra::postgres::RecordingStoreError>
    record(const infra::postgres::NewRecording& recording) = 0;
    [[nodiscard]] virtual std::expected<infra::postgres::RecordingRow,
                                        infra::postgres::RecordingStoreError>
    fail(std::string_view stream, std::string_view reason) = 0;
};

// The sandboxed ffmpeg and ffprobe the recording goes through (RecordingRemuxer).
class IRecordingCopier {
public:
    virtual ~IRecordingCopier() = default;
    [[nodiscard]] virtual std::expected<void, infra::ffmpeg::RemuxError>
    run(const infra::ffmpeg::RecordingRemuxJob& job,
        const std::function<void(std::string_view)>& on_output, const std::stop_token& stop) = 0;
    [[nodiscard]] virtual std::expected<std::optional<infra::ffmpeg::AudioFormat>,
                                        infra::ffmpeg::RemuxError>
    probe_audio(const std::filesystem::path& init, const std::filesystem::path& work_dir,
                const std::stop_token& stop) = 0;
};

} // namespace live
