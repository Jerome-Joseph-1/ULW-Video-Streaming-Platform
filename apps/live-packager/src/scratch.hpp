#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace live {

// Empties a stream's scratch directory and makes its media directory, or says why not. The
// scratch directory's parent, the scratch root, must already be a directory: the packager never
// makes it. The root must also not be a symbolic link and must be owned by the packager's user
// or by root (os::check_private_parent). The scratch directory is made, or kept, 0700 and must be
// the packager's user's (os::make_private_dir); a symbolic link or a file in its place is
// removed, a link without being followed.
[[nodiscard]] std::expected<void, std::string> clear_scratch(const std::filesystem::path& scratch);

} // namespace live
