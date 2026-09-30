#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace os {

// Makes `dir` a directory only this process's user can reach, or refuses. Each missing
// component of the path is created 0700 (so a default under a shared /var/tmp is ours from the
// start), and `dir` itself must then be a real directory, not a symbolic link, owned by the
// effective user; its mode is set to 0700 through a descriptor opened with O_NOFOLLOW, so a
// link swapped in after the check is not followed. A directory that someone else created at
// that name first is refused rather than used: what the process writes into it would be theirs
// to read or replace. The error says why, for a startup refusal.
[[nodiscard]] std::expected<void, std::string> make_private_dir(const std::filesystem::path& dir);

} // namespace os
