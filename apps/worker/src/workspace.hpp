#pragma once

#include "core/ports/random.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>

namespace worker {

enum class WorkspaceError : std::uint8_t {
    // Less than kSpaceFactor times the source is free under the scratch root.
    InsufficientSpace,
    // The scratch root cannot be read or written.
    Unavailable,
};

// Holds the source (1x), the ladder, whose bitrates sum to 8.7 Mbit/s and so stay below a
// typical upload's (up to 1x), and headroom for the filesystem and ffmpeg's temporaries.
inline constexpr std::uint64_t kSpaceFactor = 3;

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
