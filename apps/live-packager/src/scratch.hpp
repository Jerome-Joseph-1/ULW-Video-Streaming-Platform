#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace live {

// Empties a stream's scratch directory and makes its media directory, or says why not. The
// scratch directory's parent, the scratch root, must already be a directory: the packager never
// makes it.
[[nodiscard]] std::expected<void, std::string> clear_scratch(const std::filesystem::path& scratch);

} // namespace live
