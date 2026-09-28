#pragma once

#include "core/ports/clock.hpp"
#include "core/ports/transcoder.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

namespace infra::ffmpeg {

struct TranscoderConfig {
    // The ulw_sandbox helper every child is started through (ADR-0025).
    std::filesystem::path sandbox;
    std::string ffmpeg = "ffmpeg";
    std::string ffprobe = "ffprobe";
    // PATH for the children, which get no other environment.
    std::string search_path;
    // ffmpeg's -threads; left to itself it sizes by the host's cores, not the container's.
    unsigned threads = 1;
};

// ITranscoder with the ffmpeg and ffprobe programs, each run in the sandbox as a child
// process, so a decoder crash or exploit stays out of the worker (ADR-0025).
class FfmpegTranscoder final : public core::ports::ITranscoder {
public:
    FfmpegTranscoder(TranscoderConfig config, const core::ports::IClock& clock);

    [[nodiscard]] core::ports::TranscodeResult<core::ports::MediaInfo>
    probe(const std::filesystem::path& input, std::stop_token stop) override;
    [[nodiscard]] core::ports::TranscodeResult<core::ports::TranscodeStats>
    run(const std::filesystem::path& input, const std::filesystem::path& out_dir,
        const core::ports::MediaInfo& media, std::span<const core::Rung> ladder,
        core::ports::ITranscodeProgress& progress, std::stop_token stop) override;
    [[nodiscard]] core::ports::TranscodeResult<void> verify(const std::filesystem::path& out_dir,
                                                            const core::ports::MediaInfo& media,
                                                            std::span<const core::Rung> ladder,
                                                            std::stop_token stop) override;

private:
    TranscoderConfig config_;
    const core::ports::IClock& clock_;
};

// Sets the sandbox up around `writable` without running anything in it. nullopt when this
// host allows every layer of it; otherwise the helper's explanation.
[[nodiscard]] std::optional<std::string> check_sandbox(const std::filesystem::path& helper,
                                                       const std::filesystem::path& writable,
                                                       const core::ports::IClock& clock);

} // namespace infra::ffmpeg
