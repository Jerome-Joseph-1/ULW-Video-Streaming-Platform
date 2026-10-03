#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace os {

// What make_private_dir does when a directory above the one it makes is missing: makes it
// (0700), or refuses.
enum class MissingParent { Make, Refuse };

// Whether `parent` may hold a directory of this process's own: a real directory (its last
// component not a symbolic link) owned by the effective user or by root, since whoever owns it
// could rename a directory in it away and put another in its place. Its mode is not checked. The
// error says why, for a startup refusal.
[[nodiscard]] std::expected<void, std::string>
check_private_parent(const std::filesystem::path& parent);

// Makes `dir` a directory only this process's user can reach, or refuses. With
// MissingParent::Make each missing component above it is created 0700, so one made in a shared
// directory is ours from the start; with MissingParent::Refuse nothing above it is made, and a
// missing parent is refused. The parent, the directory `dir` is kept in, must pass
// check_private_parent. `dir` is then created 0700 in the parent through its descriptor, or
// taken if it is there, and must be a real directory owned by the effective user; its mode is
// set to 0700 through a descriptor opened with O_NOFOLLOW. A path ending in '/' is refused.
// The error says why, for a startup refusal.
[[nodiscard]] std::expected<void, std::string>
make_private_dir(const std::filesystem::path& dir, MissingParent missing = MissingParent::Make);

} // namespace os
