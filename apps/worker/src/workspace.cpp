#include "workspace.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace worker {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kPrefix = "job-";

std::string random_name(core::ports::IRandom& random) {
    std::array<std::byte, 8> raw{};
    random.fill(raw);
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string name(kPrefix);
    for (const std::byte b : raw) {
        name.push_back(kHex[std::to_integer<std::size_t>(b) >> 4U]);
        name.push_back(kHex[std::to_integer<std::size_t>(b) & 0xFU]);
    }
    return name;
}

} // namespace

std::expected<Workspace, WorkspaceError>
Workspace::create(const fs::path& root, std::uint64_t source_bytes, core::ports::IRandom& random) {
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::space_info space = fs::space(root, ec);
    if (ec) {
        return std::unexpected(WorkspaceError::Unavailable);
    }
    if (space.available / kSpaceFactor < source_bytes) {
        return std::unexpected(WorkspaceError::InsufficientSpace);
    }
    // 64 random bits: a collision is not expected in the lifetime of the universe, but a
    // directory that already exists is still refused rather than shared.
    fs::path dir = root / random_name(random);
    if (!fs::create_directory(dir, ec) || ec) {
        return std::unexpected(WorkspaceError::Unavailable);
    }
    fs::permissions(dir, fs::perms::owner_all, ec);
    return Workspace(std::move(dir));
}

Workspace::Workspace(Workspace&& other) noexcept : dir_(std::exchange(other.dir_, {})) {}

Workspace::~Workspace() {
    if (!dir_.empty()) {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
}

std::size_t sweep_workspaces(const fs::path& root) {
    std::vector<fs::path> leftovers;
    std::error_code ec;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (it->path().filename().string().starts_with(kPrefix) && it->is_directory(ec)) {
            leftovers.push_back(it->path());
        }
    }
    std::size_t removed = 0;
    for (const fs::path& dir : leftovers) {
        if (fs::remove_all(dir, ec) > 0 && !ec) {
            ++removed;
        }
    }
    return removed;
}

} // namespace worker
