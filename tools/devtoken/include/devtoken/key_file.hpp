#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace devtoken {

// A private JWK is about 200 bytes; anything much larger is not one.
inline constexpr std::size_t kMaxKeyFileBytes = 4096;

// Errors are errno values: EPERM for a key that is not a regular file readable by its owner
// alone, EFBIG for one over kMaxKeyFileBytes, ELOOP for a symlink.
[[nodiscard]] std::expected<std::string, int> read_key_file(const std::string& path);

// Creates `path` with mode 0600 and writes `contents`; an existing file is never replaced
// (EEXIST), and a file left half written is removed.
[[nodiscard]] std::expected<void, int> write_new_key_file(const std::string& path,
                                                          std::string_view contents);

} // namespace devtoken
