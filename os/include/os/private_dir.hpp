#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace os {

// Makes `dir` a directory only this process's user can reach, or refuses. Each missing
// component above it is created 0700, so a default under a shared /var/tmp is ours from the
// start. The parent, the directory `dir` is kept in, must be a real directory (not a symbolic
// link) owned by the effective user or by root: whoever owns it could rename `dir` away and put
// another in its place. `dir` is then created 0700 in the parent through its descriptor, or
// taken if it is there, and must be a real directory owned by the effective user; its mode is
// set to 0700 through a descriptor opened with O_NOFOLLOW. A path ending in '/' is refused.
// The error says why, for a startup refusal.
[[nodiscard]] std::expected<void, std::string> make_private_dir(const std::filesystem::path& dir);

} // namespace os
