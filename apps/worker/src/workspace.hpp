#pragma once

#include "core/models/ladder.hpp"
#include "core/ports/random.hpp"
#include "core/util/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>

namespace worker {

enum class WorkspaceError : std::uint8_t {
    // Less than kSpaceFactor times the source is free under the scratch root.
    InsufficientSpace,
    // The scratch root cannot be read or written.
    Unavailable,
};

// Holds the source (1x), the ladder, whose bitrates sum to 8.7 Mbit/s and so stay below a
// typical upload's (up to 1x), and headroom for the filesystem and ffmpeg's temporaries. A
// source below the ladder's rate is caught once its duration is known: see output_bytes.
inline constexpr std::uint64_t kSpaceFactor = 3;

// The ladder at its target rates, with its audio, for `duration`, times 5/4: maxrate caps the
// peaks at 1.07x the target, the fMP4 boxes add about 2%, and the rest covers the filesystem's
// blocks and ffmpeg's temporary playlists.
[[nodiscard]] std::uint64_t output_bytes(core::Millis duration, std::span<const core::Rung> ladder,
                                         bool has_audio) noexcept;

// Bytes free to us in the filesystem holding `dir`; nullopt when it cannot say.
using FreeSpace = std::function<std::optional<std::uint64_t>(const std::filesystem::path& dir)>;
[[nodiscard]] std::optional<std::uint64_t> free_space(const std::filesystem::path& dir);

// One job's scratch directory, removed with everything in it when the workspace goes.
class Workspace {
public:
    [[nodiscard]] static std::expected<Workspace, WorkspaceError>
    create(const std::filesystem::path& root, std::uint64_t source_bytes,
           core::ports::IRandom& random);

    ~Workspace();
    Workspace(Workspace&& other) noexcept;
    Workspace& operator=(Workspace&&) = delete;
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }
    [[nodiscard]] std::filesystem::path source() const { return dir_ / "source"; }
    [[nodiscard]] std::filesystem::path output() const { return dir_ / "hls"; }

private:
    explicit Workspace(std::filesystem::path dir) noexcept : dir_(std::move(dir)) {}

    std::filesystem::path dir_;
};

// Removes the workspaces a killed run left under `root` and returns how many. Anything else
// there is left alone.
[[nodiscard]] std::size_t sweep_workspaces(const std::filesystem::path& root);

} // namespace worker
